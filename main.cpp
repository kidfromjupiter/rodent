#include "src/bluez/BluezClient.h"
#include "src/bluez/GattServer.h"
#include "src/bluetooth/MgmtAdvertiser.h"
#include "src/clipboard/ClipboardWatcher.h"
#include "src/hid/HidReports.h"
#include "src/input/EvdevInputReader.h"
#include "src/runtime/RuntimeConfig.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_shouldStop = 0;
rodent::bluez::BluezClient* g_bluezClient = nullptr;

void handleSignal(int)
{
    if (g_bluezClient != nullptr) {
        try {
            g_bluezClient->clearAdapterAlias();
        } catch (const std::exception&) {
            // Ignore errors during shutdown
        }
    }
    g_shouldStop = 1;
}

}  // namespace

using rodent::bluez::Application;
using rodent::bluez::GattChar;
using rodent::bluez::GattDesc;
using rodent::bluez::GattManager;
using rodent::bluez::GattService;

int main()
{
    try {
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        rodent::bluez::BluezClient client;
        g_bluezClient = &client;

        try {
            client.setAdapterAlias("FabricMouse");
            std::cout << "Set adapter alias to 'FabricMouse'\n";
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to set adapter alias: " << e.what() << '\n';
        }

        const auto managedObjects = client.getManagedObjects();
        (void)managedObjects;

        auto application = Application(client.connection(), sdbus::ObjectPath("/rodent"));
        auto manager = GattManager(client.connection(), sdbus::ObjectPath("/org/bluez/hci0"));

        const std::vector<std::string> advertisement_service_uuids = {
            "00001812-0000-1000-8000-00805f9b34fb"};
        const std::string advertisement_local_name = "FabricMouse";
        constexpr uint16_t advertisement_appearance = static_cast<uint16_t>(0x03C0);
        const uint16_t bt_controller_index =
            rodent::runtime::ReadControllerIndexFromEnv(
                rodent::runtime::kEnvBtControllerIndex,
                rodent::runtime::kDefaultBtControllerIndex);
        const auto directed_target = rodent::runtime::ReadDirectedTargetConfig();
        rodent::bluetooth::MgmtAdvertiser mgmt_advertiser(bt_controller_index);

        auto batt_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service0"),
            "0000180f-0000-1000-8000-00805f9b34fb");
        batt_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        auto batt_level_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service0/char0"),
            "00002a19-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service0"),
            {100},
            {"read", "notify"});
        batt_level_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto hid_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1"),
            "00001812-0000-1000-8000-00805f9b34fb");
        hid_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        auto protocol_mode = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char0"),
            "00002a4e-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x01},
            {"read", "write-without-response"});
        protocol_mode.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto hid_info = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char1"),
            "00002a4a-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x01, 0x11, 0x00, 0x02},
            {"read"});
        hid_info.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto control_point_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char2"),
            "00002a4c-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x00},
            {"write-without-response"});
        control_point_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto report_map_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char3"),
            "00002a4b-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            rodent::hid::CompositeHidReportMap(),
            {"read"});
        report_map_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto report_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char4"),
            "00002a4d-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00},
            {"notify", "read"});
        report_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto report_desc = GattDesc(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char4/desc0"),
            "00002908-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1/char4"),
            std::vector<uint8_t>{0x01, 0x01},
            {"read"});
        report_desc.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattDescriptor1")});

        auto keyboard_report_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char5"),
            "00002a4d-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            rodent::hid::BuildKeyboardInputReport(0x00, std::array<uint8_t, rodent::hid::kKeyboardNkroBytes>{}),
            {"notify", "read"});
        keyboard_report_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto keyboard_report_desc = GattDesc(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char5/desc0"),
            "00002908-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1/char5"),
            std::vector<uint8_t>{0x02, 0x01},
            {"read"});
        keyboard_report_desc.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattDescriptor1")});

        auto device_info_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2"),
            "0000180a-0000-1000-8000-00805f9b34fb");
        device_info_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        std::string vendor_name = "kidfromjupiter";
        std::string product_name = "FabricMouse Laptop";
        auto vendor_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char0"),
            "00002a29-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t>(vendor_name.begin(), vendor_name.end()),
            {"read"});
        vendor_char.emitInterfacesAddedSignal();

        auto product_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char1"),
            "00002a24-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t>(product_name.begin(), product_name.end()),
            {"read"});
        product_char.emitInterfacesAddedSignal();

        auto pnp_id_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char2"),
            "00002a50-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t>{0x01, 0xE0, 0x00, 0x01, 0x00, 0x00, 0x01},
            {"read"});
        pnp_id_char.emitInterfacesAddedSignal();

        auto clipboard_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service3"),
            "9d1589a0-4f0e-4d3f-9a6b-4ab7d7e21f01");
        clipboard_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        auto clipboard_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service3/char0"),
            "9d1589a0-4f0e-4d3f-9a6b-4ab7d7e21f02",
            sdbus::ObjectPath("/rodent/service3"),
            std::vector<uint8_t>{},
            {"read", "notify"});
        clipboard_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});


        try {
            auto register_future = manager.RegisterApplication(sdbus::ObjectPath("/rodent"), {});
            register_future.get();
            std::cout << "Register successful\n";
        } catch (const sdbus::Error& e) {
            std::cout << "D-Bus e: " << e.getName() << ": " << e.getMessage() << '\n';
            return 1;
        } catch (const std::exception& e) {
            std::cout << "RegisterApplication failed: " << e.what() << '\n';
            return 1;
        }

        try {
            mgmt_advertiser.Start(
                advertisement_service_uuids,
                advertisement_local_name,
                advertisement_appearance,
                directed_target);
            std::cout << "Advertisement register successful via BlueZ mgmt API (hci"
                      << bt_controller_index << ")\n";
        } catch (const std::exception& e) {
            std::cout << "RegisterAdvertisement (mgmt) failed: " << e.what() << '\n';
            return 1;
        }

        std::cout << "Exported GATT application at /rodent. Waiting for Ctrl+C...\n";

        const std::string keyboard_event_path = rodent::runtime::SanitizeInputPath(
            rodent::runtime::ReadStringFromEnv(
                rodent::runtime::kEnvKeyboardEventPath,
                rodent::runtime::kDefaultKeyboardEventPath));
        const std::string mouse_event_path = rodent::runtime::SanitizeInputPath(
            rodent::runtime::ReadStringFromEnv(
                rodent::runtime::kEnvMouseEventPath,
                rodent::runtime::kDefaultMouseEventPath));
        const std::string touchpad_event_path = rodent::runtime::SanitizeInputPath(
            rodent::runtime::ReadStringFromEnv(
                rodent::runtime::kEnvTouchpadEventPath,
                rodent::runtime::kDefaultTouchpadEventPath));
        const bool grab_on_start =
            rodent::runtime::ReadBoolFromEnv(rodent::runtime::kEnvGrabOnStart, false);
        const bool clipboard_watch_primary =
            rodent::runtime::ReadBoolFromEnv(rodent::runtime::kEnvClipboardWatchPrimary, false);
        const std::string clipboard_watch_seat =
            rodent::runtime::ReadStringFromEnv(rodent::runtime::kEnvClipboardWatchSeat, "");

        const float dpi_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvDpiMultiplier, 1.0f);
        const float sensitivity_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvSensitivityMultiplier, 1.0f);
        const float touchpad_sensitivity_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvTouchpadSensitivityMultiplier, 1.0f);
        const float wheel_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvWheelMultiplier, 1.0f);
        const bool invert_scroll_direction =
            rodent::runtime::ReadBoolFromEnv(rodent::runtime::kEnvInvertScrollDirection, false);
        const float delta_multiplier = dpi_multiplier * sensitivity_multiplier;

        std::cout << "Reading evdev input from keyboard='" << keyboard_event_path
                  << "' mouse='" << mouse_event_path
                  << "' touchpad='" << touchpad_event_path
                  << "' grab_on_start=" << (grab_on_start ? "true" : "false") << '\n';
        std::cout << "Clipboard watch mode=wayland primary="
                  << (clipboard_watch_primary ? "true" : "false")
                  << " seat='" << clipboard_watch_seat << "'\n";
        std::cout << "Mouse DPI multiplier=" << dpi_multiplier
                  << " sensitivity multiplier=" << sensitivity_multiplier
                  << " effective delta multiplier=" << delta_multiplier
                  << " wheel multiplier=" << wheel_multiplier
                  << " invert scroll direction=" << (invert_scroll_direction ? "true" : "false") << '\n';
        if (!touchpad_event_path.empty()) {
            std::cout << "Touchpad sensitivity multiplier=" << touchpad_sensitivity_multiplier << '\n';
        }

        rodent::input::EvdevInputReader input_reader(keyboard_event_path, mouse_event_path, touchpad_event_path, grab_on_start, touchpad_sensitivity_multiplier);
        rodent::clipboard::WaylandClipboardWatcher clipboard_watcher({
            .seat = clipboard_watch_seat.empty() ? std::nullopt : std::optional<std::string>(clipboard_watch_seat),
            .primary = clipboard_watch_primary,
        });
        clipboard_watcher.Start();

        uint8_t current_mouse_buttons = 0;
        uint8_t last_sent_buttons = 0;
        bool has_last_sent = false;
        uint8_t current_keyboard_modifiers = 0;
        std::array<uint8_t, rodent::hid::kKeyboardNkroBytes> current_keyboard_nkro {};
        bool keyboard_dirty = false;
        std::vector<uint8_t> last_published_clipboard;
        std::set<std::string> unknown_clipboard_states_logged;

        while (!g_shouldStop) {
            const auto poll_result = input_reader.PollReports();

            if (poll_result.mouse_buttons_changed) {
                current_mouse_buttons = poll_result.mouse_buttons;
            }
            if (!poll_result.mouse_reports.empty()) {
                current_mouse_buttons = poll_result.mouse_reports.back().buttons;
            }
            keyboard_dirty = !poll_result.keyboard_states.empty();

            int coalesced_dx = 0;
            int coalesced_dy = 0;
            int coalesced_wheel = 0;
            for (const auto& report : poll_result.mouse_reports) {
                coalesced_dx += report.dx;
                coalesced_dy += report.dy;
                coalesced_wheel += report.wheel;
            }

            const int scaled_dx = rodent::runtime::ScaleDeltaWithMultiplier(coalesced_dx, delta_multiplier);
            const int scaled_dy = rodent::runtime::ScaleDeltaWithMultiplier(coalesced_dy, delta_multiplier);
            const int scaled_wheel = rodent::runtime::ScaleDeltaWithMultiplier(coalesced_wheel, wheel_multiplier);
            const int8_t dx = rodent::hid::ClampToInt8(scaled_dx);
            const int8_t dy = rodent::hid::ClampToInt8(scaled_dy);
            const int8_t wheel = rodent::hid::ClampToInt8(invert_scroll_direction ? -scaled_wheel : scaled_wheel);

            if (input_reader.GrabEnabled() && report_char.NotificationEnabled()) {
                const bool should_send_mouse =
                    (!has_last_sent || dx != 0 || dy != 0 || wheel != 0 || current_mouse_buttons != last_sent_buttons);
                if (should_send_mouse) {
                    std::cout << "HID mouse dx=" << static_cast<int>(dx)
                              << " dy=" << static_cast<int>(dy)
                              << " wheel=" << static_cast<int>(wheel)
                              << " buttons=0x" << std::hex << static_cast<int>(current_mouse_buttons) << std::dec
                              << '\n';
                    report_char.SetValueAndNotify({
                        current_mouse_buttons,
                        static_cast<uint8_t>(dx),
                        static_cast<uint8_t>(dy),
                        static_cast<uint8_t>(wheel)
                    });
                    last_sent_buttons = current_mouse_buttons;
                    has_last_sent = true;
                }
            }

            if (input_reader.GrabEnabled() && keyboard_dirty && keyboard_report_char.NotificationEnabled()) {
                for (const auto& state : poll_result.keyboard_states) {
                    current_keyboard_modifiers = state.modifiers;
                    current_keyboard_nkro = state.nkro;
                    std::cout << "HID keyboard modifiers=0x" << std::hex
                              << static_cast<int>(current_keyboard_modifiers) << std::dec << '\n';
                    keyboard_report_char.SetValueAndNotify(
                        rodent::hid::BuildKeyboardInputReport(current_keyboard_modifiers, current_keyboard_nkro));
                }
                keyboard_dirty = false;
            }

            rodent::clipboard::ClipboardEvent clipboard_event;
            while (clipboard_watcher.PollEvent(clipboard_event)) {
                std::vector<uint8_t> candidate_value;
                if (clipboard_event.state == rodent::clipboard::ClipboardState::Data) {
                    const auto bytes_opt = clipboard_watcher.ReadClipboardBytes();
                    if (!bytes_opt.has_value()) {
                        continue;
                    }
                    candidate_value = *bytes_opt;
                } else if (clipboard_event.state == rodent::clipboard::ClipboardState::Nil
                           || clipboard_event.state == rodent::clipboard::ClipboardState::Clear
                           || clipboard_event.state == rodent::clipboard::ClipboardState::Sensitive) {
                    candidate_value = {};
                } else {
                    if (unknown_clipboard_states_logged.insert(clipboard_event.raw_state).second) {
                        std::cerr << "Unknown CLIPBOARD_STATE='" << clipboard_event.raw_state << "'\n";
                    }
                    continue;
                }

                if (candidate_value.size() > clipboard_char.MaxValueLen()) {
                    continue;
                }
                if (candidate_value != last_published_clipboard) {
                    clipboard_char.SetValueAndNotify(candidate_value);

                    std::cout << "Sent clipboard contents" << std::string(candidate_value.begin(), candidate_value.end()) <<  '\n';
                    last_published_clipboard = std::move(candidate_value);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        try {
            client.clearAdapterAlias();
            std::cout << "Cleared adapter alias\n";
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to clear adapter alias: " << e.what() << '\n';
        }

        g_bluezClient = nullptr;
    } catch (const sdbus::Error& error) {
        std::cerr << "D-Bus error: " << error.getName() << ": " << error.getMessage() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
