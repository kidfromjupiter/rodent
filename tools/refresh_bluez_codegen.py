#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ast
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as et
from copy import deepcopy
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
DEFAULT_XML_ROOT = PROJECT_ROOT / "dbus" / "bluez"
DEFAULT_GENERATED_ROOT = PROJECT_ROOT / "generated" / "bluez"


def run(command: list[str]) -> str:
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    return completed.stdout


def sanitize_file_stem(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_")


def object_path_to_stem(object_path: str) -> str:
    if object_path == "/":
        return "root"
    return sanitize_file_stem(object_path.replace("/", "__"))


def parse_busctl_string(stdout: str) -> str:
    payload = stdout.strip()
    if not payload.startswith("s "):
        raise ValueError(f"Unexpected busctl output: {payload!r}")
    return ast.literal_eval(payload[2:])


def pretty_xml(element: et.Element) -> str:
    copy = deepcopy(element)
    et.indent(copy, space="  ")
    return et.tostring(copy, encoding="unicode")


def list_bluez_paths(service: str, root_path: str) -> list[str]:
    stdout = run(["busctl", "--system", "tree", "--list", service])
    return sorted(path for path in stdout.splitlines() if path == root_path or path.startswith(f"{root_path}/"))


def introspect_xml(service: str, object_path: str) -> str:
    stdout = run(
        [
            "busctl",
            "--system",
            "call",
            service,
            object_path,
            "org.freedesktop.DBus.Introspectable",
            "Introspect",
        ]
    )
    return parse_busctl_string(stdout)


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def build_interface_document(interface_element: et.Element) -> str:
    node = et.Element("node")
    node.append(deepcopy(interface_element))
    return pretty_xml(node) + "\n"


def generate_glue(interface_xml: Path, proxy_header: Path, adaptor_header: Path, xml2cpp: str) -> None:
    proxy_header.parent.mkdir(parents=True, exist_ok=True)
    adaptor_header.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            xml2cpp,
            str(interface_xml),
            f"--proxy={proxy_header}",
            f"--adaptor={adaptor_header}",
        ],
        check=True,
        text=True,
    )


def generate_umbrella_header(destination: Path, headers: list[Path], generated_root: Path) -> None:
    lines = ["#pragma once", ""]
    lines.extend(f'#include "{header.relative_to(generated_root).as_posix()}"' for header in sorted(headers))
    lines.append("")
    write_text(destination, "\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description="Dump BlueZ introspection XML and generate sdbus-c++ glue.")
    parser.add_argument("--service", default="org.bluez")
    parser.add_argument("--root-path", default="/org/bluez")
    parser.add_argument("--extra-path", action="append", default=["/"])
    parser.add_argument("--xml-root", type=Path, default=DEFAULT_XML_ROOT)
    parser.add_argument("--generated-root", type=Path, default=DEFAULT_GENERATED_ROOT)
    parser.add_argument("--xml2cpp", default="sdbus-c++-xml2cpp")
    args = parser.parse_args()

    raw_root = args.xml_root / "raw"
    interface_root = args.xml_root / "interfaces"
    proxy_root = args.generated_root / "proxies"
    adaptor_root = args.generated_root / "adaptors"

    raw_root.mkdir(parents=True, exist_ok=True)
    interface_root.mkdir(parents=True, exist_ok=True)
    proxy_root.mkdir(parents=True, exist_ok=True)
    adaptor_root.mkdir(parents=True, exist_ok=True)

    object_manifest: dict[str, dict[str, object]] = {}
    interface_manifest: dict[str, dict[str, object]] = {}
    canonical_interface_xml: dict[str, str] = {}
    proxy_headers: list[Path] = []
    adaptor_headers: list[Path] = []

    object_paths = set(list_bluez_paths(args.service, args.root_path))
    object_paths.update(args.extra_path)
    object_paths = sorted(object_paths)
    if not object_paths:
        raise RuntimeError(f"No BlueZ object paths found below {args.root_path}")

    for object_path in object_paths:
        xml = introspect_xml(args.service, object_path)
        raw_file = raw_root / f"{object_path_to_stem(object_path)}.xml"
        write_text(raw_file, xml.rstrip() + "\n")

        root = et.fromstring(xml)
        interfaces_for_object: list[str] = []
        for interface_element in root.findall("interface"):
            interface_name = interface_element.attrib["name"]
            interfaces_for_object.append(interface_name)
            interface_document = build_interface_document(interface_element)

            previous = canonical_interface_xml.get(interface_name)
            if previous is None:
                canonical_interface_xml[interface_name] = interface_document
                interface_file = interface_root / f"{interface_name}.xml"
                write_text(interface_file, interface_document)

                proxy_header = proxy_root / f"{sanitize_file_stem(interface_name)}_proxy.h"
                adaptor_header = adaptor_root / f"{sanitize_file_stem(interface_name)}_adaptor.h"
                generate_glue(interface_file, proxy_header, adaptor_header, args.xml2cpp)
                proxy_headers.append(proxy_header)
                adaptor_headers.append(adaptor_header)
                interface_manifest[interface_name] = {
                    "xml": str(interface_file.relative_to(PROJECT_ROOT)),
                    "proxy_header": str(proxy_header.relative_to(PROJECT_ROOT)),
                    "adaptor_header": str(adaptor_header.relative_to(PROJECT_ROOT)),
                    "object_paths": [object_path],
                }
            elif previous != interface_document:
                raise RuntimeError(f"Interface {interface_name} differs across object paths; manual merge required")
            else:
                interface_manifest[interface_name]["object_paths"].append(object_path)

        object_manifest[object_path] = {
            "raw_xml": str(raw_file.relative_to(PROJECT_ROOT)),
            "interfaces": sorted(interfaces_for_object),
        }

    generate_umbrella_header(args.generated_root / "bluez_all_proxies.h", proxy_headers, args.generated_root)
    generate_umbrella_header(args.generated_root / "bluez_all_adaptors.h", adaptor_headers, args.generated_root)

    manifest = {
        "service": args.service,
        "root_path": args.root_path,
        "object_paths": object_manifest,
        "interfaces": interface_manifest,
    }
    write_text(args.xml_root / "manifest.json", json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    print(f"Dumped {len(object_manifest)} object XML files and generated {len(interface_manifest)} interface bindings.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        if error.stderr:
            sys.stderr.write(error.stderr)
        raise
