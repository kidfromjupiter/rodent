#!/usr/bin/env python3
"""
Send test mouse frames to rodent's Unix socket input.

Protocol:
  MOVE    -> [type=0x01][dx:int16 big-endian][dy:int16 big-endian]
  BUTTONS -> [type=0x02][mask:uint8]
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time
from dataclasses import dataclass


PACKET_TYPE_MOVE = 0x01
PACKET_TYPE_BUTTONS = 0x02

LEFT_BUTTON = 0x01
RIGHT_BUTTON = 0x02
MIDDLE_BUTTON = 0x04


def build_move_frame(dx: int, dy: int) -> bytes:
    if dx < -32768 or dx > 32767 or dy < -32768 or dy > 32767:
        raise ValueError(f"dx/dy out of int16 range: dx={dx} dy={dy}")
    return struct.pack(">Bhh", PACKET_TYPE_MOVE, dx, dy)


def build_button_frame(mask: int) -> bytes:
    if mask < 0 or mask > 0xFF:
        raise ValueError(f"button mask out of uint8 range: {mask}")
    return struct.pack("BB", PACKET_TYPE_BUTTONS, mask)


@dataclass
class MoveConfig:
    pattern: str
    amp_x: int
    amp_y: int
    step_x: int
    step_y: int
    period_s: float


@dataclass
class ButtonConfig:
    pattern: str
    period_s: float
    hold_s: float


class MovePatternGenerator:
    def __init__(self, cfg: MoveConfig) -> None:
        self.cfg = cfg
        self.tick = 0

    def next(self, now_s: float) -> tuple[int, int]:
        p = self.cfg.pattern
        self.tick += 1
        if p == "none":
            return (0, 0)
        if p == "line-x":
            return (self.cfg.step_x, 0)
        if p == "line-y":
            return (0, self.cfg.step_y)
        if p == "zigzag":
            sign = 1 if (self.tick // 8) % 2 == 0 else -1
            return (sign * self.cfg.step_x, self.cfg.step_y // 3)
        if p == "square":
            quarter = max(1, int(self.cfg.period_s * 1000) // 4)
            side = (self.tick // quarter) % 4
            if side == 0:
                return (self.cfg.step_x, 0)
            if side == 1:
                return (0, self.cfg.step_y)
            if side == 2:
                return (-self.cfg.step_x, 0)
            return (0, -self.cfg.step_y)
        if p == "circle":
            omega = (2.0 * math.pi) / max(0.001, self.cfg.period_s)
            phase = omega * now_s
            dx = int(round(self.cfg.amp_x * math.cos(phase)))
            dy = int(round(self.cfg.amp_y * math.sin(phase)))
            return (dx, dy)
        if p == "burst":
            if self.tick % 20 == 0:
                return (self.cfg.amp_x, self.cfg.amp_y)
            return (0, 0)
        raise ValueError(f"Unknown move pattern: {p}")


class ButtonPatternGenerator:
    def __init__(self, cfg: ButtonConfig) -> None:
        self.cfg = cfg
        self.last_mask = 0
        self.next_toggle_at = 0.0
        self.release_at = 0.0
        self.seq = [LEFT_BUTTON, RIGHT_BUTTON, MIDDLE_BUTTON]
        self.seq_idx = 0

    def next(self, now_s: float) -> int | None:
        p = self.cfg.pattern
        if p == "none":
            return None
        if p == "hold-left":
            return LEFT_BUTTON if self.last_mask != LEFT_BUTTON else None
        if p == "click-left":
            if now_s >= self.next_toggle_at:
                if self.last_mask == 0:
                    self.last_mask = LEFT_BUTTON
                    self.release_at = now_s + max(0.001, self.cfg.hold_s)
                    self.next_toggle_at = now_s + max(0.001, self.cfg.period_s)
                    return self.last_mask
                if now_s >= self.release_at:
                    self.last_mask = 0
                    return self.last_mask
            return None
        if p == "toggle-left":
            if now_s >= self.next_toggle_at:
                self.next_toggle_at = now_s + max(0.001, self.cfg.period_s)
                self.last_mask = 0 if self.last_mask else LEFT_BUTTON
                return self.last_mask
            return None
        if p == "cycle":
            if now_s >= self.next_toggle_at:
                self.next_toggle_at = now_s + max(0.001, self.cfg.period_s)
                self.last_mask = self.seq[self.seq_idx]
                self.seq_idx = (self.seq_idx + 1) % len(self.seq)
                return self.last_mask
            return None
        raise ValueError(f"Unknown button pattern: {p}")


def connect_with_retry(path: str, timeout_s: float) -> socket.socket:
    deadline = time.monotonic() + timeout_s
    last_err: Exception | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            return sock
        except OSError as exc:
            last_err = exc
            time.sleep(0.1)
    raise RuntimeError(f"Failed to connect to socket {path}: {last_err}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send test MOVE/BUTTON frames to rodent Unix socket input."
    )
    parser.add_argument("--socket", default="/tmp/hyprfabric-nearby.sock", help="Unix socket path")
    parser.add_argument("--hz", type=float, default=125.0, help="Frame frequency in Hz")
    parser.add_argument("--duration", type=float, default=0.0, help="Run duration in seconds; 0 = infinite")
    parser.add_argument("--connect-timeout", type=float, default=5.0, help="Socket connect timeout in seconds")

    parser.add_argument(
        "--move-pattern",
        choices=["none", "line-x", "line-y", "zigzag", "square", "circle", "burst"],
        default="circle",
    )
    parser.add_argument("--amp-x", type=int, default=20, help="Movement amplitude X for circle/burst")
    parser.add_argument("--amp-y", type=int, default=20, help="Movement amplitude Y for circle/burst")
    parser.add_argument("--step-x", type=int, default=6, help="Step X for line/zigzag/square")
    parser.add_argument("--step-y", type=int, default=6, help="Step Y for line/zigzag/square")
    parser.add_argument("--move-period", type=float, default=2.0, help="Seconds per cycle for circle/square")

    parser.add_argument(
        "--button-pattern",
        choices=["none", "hold-left", "click-left", "toggle-left", "cycle"],
        default="none",
    )
    parser.add_argument("--button-period", type=float, default=0.5, help="Seconds between button state changes")
    parser.add_argument("--button-hold", type=float, default=0.05, help="Hold time for click-left")

    parser.add_argument("--quiet", action="store_true", help="Suppress periodic stats output")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.hz <= 0:
        raise ValueError("--hz must be > 0")

    move_cfg = MoveConfig(
        pattern=args.move_pattern,
        amp_x=args.amp_x,
        amp_y=args.amp_y,
        step_x=args.step_x,
        step_y=args.step_y,
        period_s=args.move_period,
    )
    btn_cfg = ButtonConfig(
        pattern=args.button_pattern,
        period_s=args.button_period,
        hold_s=args.button_hold,
    )

    move_gen = MovePatternGenerator(move_cfg)
    btn_gen = ButtonPatternGenerator(btn_cfg)

    sock = connect_with_retry(args.socket, args.connect_timeout)
    interval = 1.0 / args.hz
    next_tick = time.monotonic()
    start = next_tick
    last_stat = start
    sent_frames = 0

    print(
        f"Connected to {args.socket} | hz={args.hz:.2f} | move={args.move_pattern} | "
        f"buttons={args.button_pattern}"
    )

    try:
        while True:
            now = time.monotonic()
            if args.duration > 0 and now - start >= args.duration:
                break

            dx, dy = move_gen.next(now)
            if dx != 0 or dy != 0:
                sock.sendall(build_move_frame(dx, dy))
                sent_frames += 1

            maybe_mask = btn_gen.next(now)
            if maybe_mask is not None:
                sock.sendall(build_button_frame(maybe_mask))
                sent_frames += 1

            if not args.quiet and now - last_stat >= 1.0:
                elapsed = max(1e-6, now - start)
                print(f"sent={sent_frames} frames | avg={sent_frames / elapsed:.1f} fps")
                last_stat = now

            next_tick += interval
            sleep_s = next_tick - time.monotonic()
            if sleep_s > 0:
                time.sleep(sleep_s)
            else:
                next_tick = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            sock.close()
        except OSError:
            pass

    print(f"Done. Sent {sent_frames} frames.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
