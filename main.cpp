#include "src/bluez/BluezClient.h"
#include "src/bluez/GattServer.h"
#include "src/bluetooth/MgmtAdvertiser.h"
#include "src/hid/HidReports.h"
#include "src/input/EvdevInputReader.h"
#include "src/runtime/RuntimeConfig.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_shouldStop = 0;

void handleSignal(int)
{
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
        std::string product_name = "FabricMouse";
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
        const bool grab_on_start =
            rodent::runtime::ReadBoolFromEnv(rodent::runtime::kEnvGrabOnStart, false);

        std::cout << "Reading evdev input from keyboard='" << keyboard_event_path
                  << "' mouse='" << mouse_event_path
                  << "' grab_on_start=" << (grab_on_start ? "true" : "false") << '\n';

        rodent::input::EvdevInputReader input_reader(keyboard_event_path, mouse_event_path, grab_on_start);
        const float dpi_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvDpiMultiplier, 1.0f);
        const float sensitivity_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvSensitivityMultiplier, 1.0f);
        const float wheel_multiplier =
            rodent::runtime::ReadMultiplierFromEnv(rodent::runtime::kEnvWheelMultiplier, 1.0f);
        const bool invert_scroll_direction =
            rodent::runtime::ReadBoolFromEnv(rodent::runtime::kEnvInvertScrollDirection, false);
        const float delta_multiplier = dpi_multiplier * sensitivity_multiplier;

        std::cout << "Mouse DPI multiplier=" << dpi_multiplier
                  << " sensitivity multiplier=" << sensitivity_multiplier
                  << " effective delta multiplier=" << delta_multiplier
                  << " wheel multiplier=" << wheel_multiplier
                  << " invert scroll direction=" << (invert_scroll_direction ? "true" : "false") << '\n';

        uint8_t current_mouse_buttons = 0;
        uint8_t last_sent_buttons = 0;
        bool has_last_sent = false;
        uint8_t current_keyboard_modifiers = 0;
        std::array<uint8_t, rodent::hid::kKeyboardNkroBytes> current_keyboard_nkro {};
        bool keyboard_dirty = false;

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

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const sdbus::Error& error) {
        std::cerr << "D-Bus error: " << error.getName() << ": " << error.getMessage() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
