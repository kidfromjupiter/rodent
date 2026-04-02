#include "src/bluez/BluezClient.h"
#include "src/bluez/GattServer.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <cerrno>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <limits>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    volatile std::sig_atomic_t g_shouldStop = 0;
    constexpr const char* kInputSocketPath = "/tmp/hyprfabric-nearby.sock";
    constexpr uint8_t kPacketTypeMove = 0x01;
    constexpr uint8_t kPacketTypeButtons = 0x02;
    constexpr uint8_t kPacketTypeKeyDown = 0x03;
    constexpr uint8_t kPacketTypeKeyUp = 0x04;
    constexpr uint8_t kPacketTypeModifiers = 0x05;
    constexpr uint8_t kPacketTypeScroll = 0x06;
    constexpr uint8_t kKeyboardUsageMin = 0x04;
    constexpr uint8_t kKeyboardUsageMax = 0xE7;
    constexpr std::size_t kKeyboardNkroBytes = 29;
    constexpr const char* kEnvDpiMultiplier = "RODENT_DPI_MULTIPLIER";
    constexpr const char* kEnvSensitivityMultiplier = "RODENT_SENSITIVITY_MULTIPLIER";
    constexpr const char* kEnvWheelMultiplier = "RODENT_WHEEL_MULTIPLIER";
    constexpr const char* kEnvInvertScrollDirection = "RODENT_INVERT_SCROLL_DIRECTION";

    void handleSignal(int)
    {
        g_shouldStop = 1;
    }

    int16_t readInt16BE(const uint8_t* data)
    {
        const uint16_t raw = static_cast<uint16_t>(data[0]) << 8 | static_cast<uint16_t>(data[1]);
        return static_cast<int16_t>(raw);
    }

    int8_t clampToInt8(int value)
    {
        if (value < -127) {
            std::cout << "CLAMPED\n";
            return -127;
        }
        if (value > 127) {
            std::cout << "CLAMPED\n";
            return 127;
        }
        return static_cast<int8_t>(value);
    }

    bool isKeyboardUsageInRange(uint8_t usage)
    {
        return usage >= kKeyboardUsageMin && usage <= kKeyboardUsageMax;
    }

    bool setKeyboardUsageBit(std::array<uint8_t, kKeyboardNkroBytes>& bitmap, uint8_t usage, bool pressed)
    {
        if (!isKeyboardUsageInRange(usage)) {
            return false;
        }

        const std::size_t index = static_cast<std::size_t>(usage - kKeyboardUsageMin);
        const std::size_t byte_index = index / 8;
        const uint8_t bit_mask = static_cast<uint8_t>(1u << (index % 8));
        const bool was_set = (bitmap[byte_index] & bit_mask) != 0;

        if (pressed) {
            bitmap[byte_index] = static_cast<uint8_t>(bitmap[byte_index] | bit_mask);
        } else {
            bitmap[byte_index] = static_cast<uint8_t>(bitmap[byte_index] & ~bit_mask);
        }

        return was_set != pressed;
    }

    float readMultiplierFromEnv(const char* env_name, float default_value)
    {
        const char* raw = std::getenv(env_name);
        if (raw == nullptr || raw[0] == '\0') {
            return default_value;
        }

        char* end = nullptr;
        errno = 0;
        const float parsed = std::strtof(raw, &end);
        if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || !std::isfinite(parsed) || parsed <= 0.0f) {
            std::cerr << "Invalid " << env_name << "='" << raw << "', defaulting to " << default_value << '\n';
            return default_value;
        }

        return parsed;
    }

    int scaleDeltaWithMultiplier(int delta, float multiplier)
    {
        const double scaled = static_cast<double>(delta) * static_cast<double>(multiplier);
        if (scaled < static_cast<double>(std::numeric_limits<int>::min())) {
            return std::numeric_limits<int>::min();
        }
        if (scaled > static_cast<double>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(std::lround(scaled));
    }

    bool readBoolFromEnv(const char* env_name, bool default_value)
    {
        const char* raw = std::getenv(env_name);
        if (raw == nullptr || raw[0] == '\0') {
            return default_value;
        }

        std::string normalized(raw);
        std::transform(
            normalized.begin(),
            normalized.end(),
            normalized.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
            return false;
        }

        std::cerr << "Invalid " << env_name << "='" << raw << "', defaulting to "
                  << (default_value ? "true" : "false") << '\n';
        return default_value;
    }

    class UnixInputReader {
    public:
        struct KeyboardState {
            uint8_t modifiers = 0;
            std::array<uint8_t, kKeyboardNkroBytes> nkro {};
        };

        struct PollResult {
            std::vector<std::array<uint8_t, 4>> mouse_reports;
            bool mouse_buttons_changed = false;
            uint8_t mouse_buttons = 0;
            std::vector<KeyboardState> keyboard_states;
        };

        explicit UnixInputReader(std::string socketPath)
            : socket_path_(std::move(socketPath))
        {
            listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (listen_fd_ < 0) {
                throw std::runtime_error("Failed to create Unix socket");
            }

            ::unlink(socket_path_.c_str());

            sockaddr_un addr {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                throw std::runtime_error(std::string("Failed to bind Unix socket: ") + std::strerror(errno));
            }
            if (::listen(listen_fd_, 1) < 0) {
                throw std::runtime_error(std::string("Failed to listen on Unix socket: ") + std::strerror(errno));
            }
        }

        ~UnixInputReader()
        {
            if (client_fd_ >= 0) {
                ::close(client_fd_);
                client_fd_ = -1;
            }
            if (listen_fd_ >= 0) {
                ::close(listen_fd_);
                listen_fd_ = -1;
            }
            ::unlink(socket_path_.c_str());
        }

        PollResult PollReports()
        {
            acceptClientIfNeeded();
            return readClientReports();
        }

    private:
        void acceptClientIfNeeded()
        {
            if (client_fd_ >= 0) {
                return;
            }

            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
            if (fd >= 0) {
                client_fd_ = fd;
                std::cout << "Unix input client connected\n";
                return;
            }

            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "accept4 failed: " << std::strerror(errno) << '\n';
            }
        }

        PollResult readClientReports()
        {
            PollResult result {};
            result.mouse_buttons = button_mask_;
            if (client_fd_ < 0) {
                return result;
            }

            uint8_t chunk[256];
            while (true) {
                const ssize_t n = ::recv(client_fd_, chunk, sizeof(chunk), 0);
                if (n > 0) {
                    std::size_t cursor = 0;
                    while (cursor < static_cast<std::size_t>(n)) {
                        const uint8_t type = chunk[cursor];
                        std::size_t frameSize = 0;
                        if (type == kPacketTypeMove || type == kPacketTypeScroll) {
                            frameSize = 5;
                        } else if (type == kPacketTypeButtons) {
                            frameSize = 2;
                        } else if (type == kPacketTypeKeyDown || type == kPacketTypeKeyUp || type == kPacketTypeModifiers) {
                            frameSize = 2;
                        } else {
                            ++cursor;
                            continue;
                        }

                        if (static_cast<std::size_t>(n) - cursor < frameSize) {
                            std::cout << "Dropped partial frame\n";
                            break;
                        }

                        if (type == kPacketTypeMove) {
                            const int16_t dx16 = readInt16BE(chunk + cursor + 1);
                            const int16_t dy16 = readInt16BE(chunk + cursor + 3);
                            const int8_t dx = clampToInt8(dx16);
                            const int8_t dy = clampToInt8(dy16);

                            if (dx != 0 || dy != 0) {
                                result.mouse_reports.push_back({
                                    button_mask_,
                                    static_cast<uint8_t>(dx),
                                    static_cast<uint8_t>(dy),
                                    0x00
                                });
                            }
                        } else if (type == kPacketTypeScroll) {
                            const int16_t horizontal16 = readInt16BE(chunk + cursor + 1);
                            const int16_t vertical16 = readInt16BE(chunk + cursor + 3);
                            const int8_t horizontal = clampToInt8(horizontal16);
                            const int8_t vertical = clampToInt8(vertical16);

                            if (horizontal != 0) {
                                std::cout << "Ignoring horizontal scroll delta=" << static_cast<int>(horizontal)
                                          << " (descriptor exposes vertical wheel only)\n";
                            }
                            if (vertical != 0) {
                                result.mouse_reports.push_back({
                                    button_mask_,
                                    0x00,
                                    0x00,
                                    static_cast<uint8_t>(vertical)
                                });
                            }
                        } else if (type == kPacketTypeButtons) {
                            button_mask_ = chunk[cursor + 1];
                            result.mouse_buttons = button_mask_;
                            result.mouse_buttons_changed = true;
                            result.mouse_reports.push_back({
                                button_mask_,
                                0x00,
                                0x00,
                                0x00
                            });
                        } else if (type == kPacketTypeModifiers) {
                            const uint8_t new_modifiers = chunk[cursor + 1];
                            if (keyboard_modifiers_ != new_modifiers) {
                                keyboard_modifiers_ = new_modifiers;
                                result.keyboard_states.push_back({keyboard_modifiers_, keyboard_nkro_});
                            }
                        } else {
                            const uint8_t usage = chunk[cursor + 1];
                            if (!isKeyboardUsageInRange(usage)) {
                                std::cout << "Ignored keyboard usage 0x" << std::hex << static_cast<int>(usage) << std::dec << '\n';
                            } else {
                                const bool changed = setKeyboardUsageBit(keyboard_nkro_, usage, type == kPacketTypeKeyDown);
                                if (changed) {
                                    result.keyboard_states.push_back({keyboard_modifiers_, keyboard_nkro_});
                                }
                            }
                        }

                        cursor += frameSize;
                    }
                    result.mouse_buttons = button_mask_;
                    continue;
                }

                if (n == 0) {
                    ::close(client_fd_);
                    client_fd_ = -1;
                    std::cout << "Unix input client disconnected\n";
                    return result;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return result;
                }

                std::cerr << "recv failed: " << std::strerror(errno) << '\n';
                ::close(client_fd_);
                client_fd_ = -1;
                return result;
            }
        }

        std::string socket_path_;
        int listen_fd_ = -1;
        int client_fd_ = -1;
        uint8_t button_mask_ = 0;
        uint8_t keyboard_modifiers_ = 0;
        std::array<uint8_t, kKeyboardNkroBytes> keyboard_nkro_ {};
    };
}

using rodent::bluez::AdvertisingManager;
using rodent::bluez::Application;
using rodent::bluez::GattChar;
using rodent::bluez::GattDesc;
using rodent::bluez::GattManager;
using rodent::bluez::GattService;
using rodent::bluez::LEAdvertisement;

static const std::vector<uint8_t> composite_hid_report_map {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        // Report ID(1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)

    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)

    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x03,        //     Input (Const,Var,Abs) - padding

    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data,Var,Rel)

    0xC0,              //   End Collection
    0xC0,              // End Collection

    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Keyboard LeftControl)
    0x29, 0xE7,        //   Usage Maximum (Keyboard Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs) - modifiers
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Array,Abs) - reserved
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x04,        //   Usage Minimum (Keyboard 'a' and 'A')
    0x29, 0xE7,        //   Usage Maximum (Keyboard Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0xE4,        //   Report Count (228)
    0x81, 0x02,        //   Input (Data,Var,Abs) - NKRO bitmap
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x04,        //   Report Count (4)
    0x81, 0x01,        //   Input (Const,Array,Abs) - padding
    0xC0               // End Collection
};

std::vector<uint8_t> buildKeyboardInputReport(
    uint8_t modifiers,
    const std::array<uint8_t, kKeyboardNkroBytes>& nkro)
{
    std::vector<uint8_t> report;
    report.reserve(2 + nkro.size());
    report.push_back(modifiers);
    report.push_back(0x00);
    report.insert(report.end(), nkro.begin(), nkro.end());
    return report;
}

int main()
{
    try {
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        rodent::bluez::BluezClient client;
        const auto managedObjects = client.getManagedObjects();

        auto application = Application(client.connection(), sdbus::ObjectPath("/rodent"));
        auto manager = GattManager(client.connection(), sdbus::ObjectPath("/org/bluez/hci0"));
        auto adv_manager = AdvertisingManager(client.connection(), sdbus::ObjectPath("/org/bluez/hci0"));

        LEAdvertisement::Config advertisement_config;
        advertisement_config.type = "peripheral";
        advertisement_config.service_uuids = {"00001812-0000-1000-8000-00805f9b34fb"};
        // advertisement_config.service_data = {{"00001812-0000-1000-8000-00805f9b34fb",
        //     sdbus::Variant(std::vector<uint8_t>{0x05})}};
        advertisement_config.local_name = "FabricMouse";
        advertisement_config.appearance = static_cast<uint16_t>(0x03C0);
        advertisement_config.min_interval_ms = 30;
        advertisement_config.max_interval_ms = 50;
        auto advertisement = LEAdvertisement(client.connection(), sdbus::ObjectPath("/rodent/advert0"), std::move(advertisement_config));
        // add battery service
        auto batt_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service0" ),
            "0000180f-0000-1000-8000-00805f9b34fb"
        );
        batt_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        auto batt_level_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service0/char0"),
            "00002a19-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service0"),
            {100},
            {"read", "notify"}

        );
        batt_level_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        // add HID Service
        auto hid_service = GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1"),
            "00001812-0000-1000-8000-00805f9b34fb"
            );
        hid_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});

        // TODO: add HID chars
        // protocolmode, HIDInfoChars, ControlPointChars, ReportMapChars, ReportMapj

        auto protocol_mode = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char0"),
            "00002a4e-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x01},
            {"read", "write-without-response"}

        );
        protocol_mode.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto hid_info = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char1"),
            "00002a4a-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x01, 0x11, 0x00, 0x02},
            {"read"}

        );
        hid_info.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto control_point_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char2"),
            "00002a4c-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x00},
            {"write-without-response"}

        );
        control_point_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto report_map_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char3"),
            "00002a4b-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            composite_hid_report_map,
            {"read"}

        );

        report_map_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});
        auto report_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char4"),
            "00002a4d-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            std::vector<uint8_t>{0x00, 0x00, 0x00, 0x00},
            {"notify", "read"}

        );
        report_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto report_desc = GattDesc(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char4/desc0"),
            "00002908-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1/char4"),
            std::vector<uint8_t>{0x01,0x01},
            {"read"}
        );
        report_desc.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattDescriptor1")});

        auto keyboard_report_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char5"),
            "00002a4d-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1"),
            buildKeyboardInputReport(0x00, std::array<uint8_t, kKeyboardNkroBytes>{}),
            {"notify", "read"}
        );
        keyboard_report_char.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattCharacteristic1")});

        auto keyboard_report_desc = GattDesc(
            client.connection(),
            sdbus::ObjectPath("/rodent/service1/char5/desc0"),
            "00002908-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service1/char5"),
            std::vector<uint8_t>{0x02,0x01},
            {"read"}
        );
        keyboard_report_desc.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattDescriptor1")});

        // add DeviceInfoService
        auto device_info_service =GattService(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2"),
            "0000180a-0000-1000-8000-00805f9b34fb"
        );
        device_info_service.emitInterfacesAddedSignal({sdbus::InterfaceName("org.bluez.GattService1")});
        // TODO: add DeviceInfo chars.
        std::string vendor_name = "kidfromjupiter";
        std::string product_name = "FabricMouse";
        auto vendor_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char0"),
            "00002a29-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t> (vendor_name.begin(), vendor_name.end()),
            {"read"}
        );
        vendor_char.emitInterfacesAddedSignal();


        auto product_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char1"),
            "00002a24-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t> (product_name.begin(), product_name.end()),
            {"read"}
        );
        product_char.emitInterfacesAddedSignal();

        // PnP ID (2A50): [Vendor ID Source, Vendor ID LSB, Vendor ID MSB, Product ID LSB,
        // Product ID MSB, Product Version LSB, Product Version MSB]
        auto pnp_id_char = GattChar(
            client.connection(),
            sdbus::ObjectPath("/rodent/service2/char2"),
            "00002a50-0000-1000-8000-00805f9b34fb",
            sdbus::ObjectPath("/rodent/service2"),
            std::vector<uint8_t>{0x01, 0xE0, 0x00, 0x01, 0x00, 0x00, 0x01},
            {"read"}
        );
        pnp_id_char.emitInterfacesAddedSignal();


        try
        {
            auto register_future = manager.RegisterApplication(sdbus::ObjectPath("/rodent"), {});
            register_future.get();
            std::cout << "Register successful\n";
        } catch (const sdbus::Error& e)
        {
            std::cout << "D-Bus e: " << e.getName() << ": " << e.getMessage() << '\n';
            return 1;
        } catch (const std::exception& e)
        {
            std::cout << "RegisterApplication failed: " << e.what() << '\n';
            return 1;
        }

        try
        {
            auto register_adv_pending = adv_manager.RegisterAdvertisementAsync(sdbus::ObjectPath("/rodent/advert0"), {});
            (void)register_adv_pending;
            auto register_adv_error = adv_manager.WaitForRegisterAdvertisement();
            if (register_adv_error.has_value()) {
                std::cout << "RegisterAdvertisement D-Bus e: "
                          << register_adv_error->getName() << ": "
                          << register_adv_error->getMessage() << '\n';
                return 1;
            }

            std::cout << "Advertisement register successful\n";
        } catch (const std::exception& e)
        {
            std::cout << "RegisterAdvertisement failed: " << e.what() << '\n';
            return 1;
        }
        std::cout << "Exported GATT application at /rodent. Waiting for Ctrl+C...\n";
        std::cout << "Listening for Unix input on " << kInputSocketPath << '\n';
        UnixInputReader input_reader(kInputSocketPath);
        const float dpi_multiplier = readMultiplierFromEnv(kEnvDpiMultiplier, 1.0f);
        const float sensitivity_multiplier = readMultiplierFromEnv(kEnvSensitivityMultiplier, 1.0f);
        const float wheel_multiplier = readMultiplierFromEnv(kEnvWheelMultiplier, 1.0f);
        const bool invert_scroll_direction = readBoolFromEnv(kEnvInvertScrollDirection, false);
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
        std::array<uint8_t, kKeyboardNkroBytes> current_keyboard_nkro {};
        bool keyboard_dirty = false;

        while (!g_shouldStop) {
            const auto poll_result = input_reader.PollReports();

            if (poll_result.mouse_buttons_changed) {
                current_mouse_buttons = poll_result.mouse_buttons;
            }
            if (!poll_result.mouse_reports.empty()) {
                current_mouse_buttons = poll_result.mouse_reports.back()[0];
            }
            keyboard_dirty = !poll_result.keyboard_states.empty();

            int coalesced_dx = 0;
            int coalesced_dy = 0;
            int coalesced_wheel = 0;
            for (const auto& report : poll_result.mouse_reports) {
                coalesced_dx += static_cast<int>(static_cast<int8_t>(report[1]));
                coalesced_dy += static_cast<int>(static_cast<int8_t>(report[2]));
                coalesced_wheel += static_cast<int>(static_cast<int8_t>(report[3]));
            }
            const int scaled_dx = scaleDeltaWithMultiplier(coalesced_dx, delta_multiplier);
            const int scaled_dy = scaleDeltaWithMultiplier(coalesced_dy, delta_multiplier);
            const int scaled_wheel = scaleDeltaWithMultiplier(coalesced_wheel, wheel_multiplier);
            const int8_t dx = clampToInt8(scaled_dx);
            const int8_t dy = clampToInt8(scaled_dy);
            const int8_t wheel = clampToInt8(invert_scroll_direction ? -scaled_wheel : scaled_wheel);

            if (report_char.NotificationEnabled()) {
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

            if (keyboard_dirty && keyboard_report_char.NotificationEnabled()) {
                for (const auto& state : poll_result.keyboard_states) {
                    current_keyboard_modifiers = state.modifiers;
                    current_keyboard_nkro = state.nkro;
                    std::cout << "HID keyboard modifiers=0x" << std::hex
                              << static_cast<int>(current_keyboard_modifiers) << std::dec << '\n';
                    keyboard_report_char.SetValueAndNotify(
                        buildKeyboardInputReport(current_keyboard_modifiers, current_keyboard_nkro));
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
