#include "src/bluez/BluezClient.h"
#include "src/bluez/GattServer.h"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>

#include <fcntl.h>
#include <bluetooth/hci.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    volatile std::sig_atomic_t g_shouldStop = 0;
    constexpr const char* kDefaultKeyboardEventPath = "/dev/input/by-id/usb-BY_Tech_Gaming_Keyboard-event-kbd";
    constexpr const char* kDefaultMouseEventPath = "/dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse";
    constexpr uint8_t kKeyboardUsageMin = 0x04;
    constexpr uint8_t kKeyboardUsageMax = 0xE7;
    constexpr std::size_t kKeyboardNkroBytes = 29;
    constexpr const char* kEnvDpiMultiplier = "RODENT_DPI_MULTIPLIER";
    constexpr const char* kEnvSensitivityMultiplier = "RODENT_SENSITIVITY_MULTIPLIER";
    constexpr const char* kEnvWheelMultiplier = "RODENT_WHEEL_MULTIPLIER";
    constexpr const char* kEnvInvertScrollDirection = "RODENT_INVERT_SCROLL_DIRECTION";
    constexpr const char* kEnvKeyboardEventPath = "RODENT_EVDEV_KEYBOARD_PATH";
    constexpr const char* kEnvMouseEventPath = "RODENT_EVDEV_MOUSE_PATH";
    constexpr const char* kEnvGrabOnStart = "RODENT_GRAB_ON_START";
    constexpr const char* kEnvDirectedAdvPeer = "RODENT_DIRECTED_ADV_PEER";
    constexpr const char* kEnvMgmtIndex = "RODENT_MGMT_INDEX";
    constexpr uint16_t kMgmtOpAddDevice = 0x0033;
    constexpr uint16_t kMgmtEvCmdComplete = 0x0001;
    constexpr uint16_t kMgmtEvCmdStatus = 0x0002;
    constexpr uint8_t kMgmtStatusSuccess = 0x00;
    constexpr uint8_t kMgmtAddrTypeLePublic = 0x01;
    constexpr uint8_t kMgmtActionAllowConnection = 0x01;

    struct mgmt_hdr {
        uint16_t opcode;
        uint16_t index;
        uint16_t len;
    } __attribute__((packed));

    struct mgmt_cp_add_device {
        uint8_t addr[6];
        uint8_t addr_type;
        uint8_t action;
    } __attribute__((packed));

    struct mgmt_ev_cmd_complete {
        uint16_t opcode;
        uint8_t status;
    } __attribute__((packed));

    struct mgmt_ev_cmd_status {
        uint8_t status;
        uint16_t opcode;
    } __attribute__((packed));

    void handleSignal(int)
    {
        g_shouldStop = 1;
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

    std::string readStringFromEnv(const char* env_name, const char* default_value)
    {
        const char* raw = std::getenv(env_name);
        if (raw == nullptr || raw[0] == '\0') {
            return default_value;
        }
        return raw;
    }

    std::string sanitizeInputPath(std::string path)
    {
        while (!path.empty() && path.back() == '@') {
            path.pop_back();
        }
        return path;
    }

    constexpr std::size_t bitsToULongs(std::size_t bits)
    {
        return (bits + (sizeof(unsigned long) * 8) - 1) / (sizeof(unsigned long) * 8);
    }

    bool bitIsSet(const std::vector<unsigned long>& bits, std::size_t bit)
    {
        const std::size_t word_bits = sizeof(unsigned long) * 8;
        const std::size_t idx = bit / word_bits;
        const std::size_t offset = bit % word_bits;
        if (idx >= bits.size()) {
            return false;
        }
        return (bits[idx] & (1UL << offset)) != 0;
    }

    std::vector<unsigned long> queryBitset(int fd, unsigned int ev, std::size_t max_bit)
    {
        const std::size_t n_longs = bitsToULongs(max_bit + 1);
        std::vector<unsigned long> bits(n_longs, 0UL);
        const std::size_t n_bytes = n_longs * sizeof(unsigned long);
        if (::ioctl(fd, EVIOCGBIT(ev, n_bytes), bits.data()) < 0) {
            return {};
        }
        return bits;
    }

    bool isKeyboardDevice(int fd)
    {
        auto ev_bits = queryBitset(fd, 0, EV_MAX);
        if (ev_bits.empty() || !bitIsSet(ev_bits, EV_KEY)) {
            return false;
        }

        auto key_bits = queryBitset(fd, EV_KEY, KEY_MAX);
        if (key_bits.empty()) {
            return false;
        }

        return bitIsSet(key_bits, KEY_A) && bitIsSet(key_bits, KEY_Z) && bitIsSet(key_bits, KEY_LEFTCTRL);
    }

    bool isMouseDevice(int fd)
    {
        auto ev_bits = queryBitset(fd, 0, EV_MAX);
        if (ev_bits.empty() || !bitIsSet(ev_bits, EV_REL) || !bitIsSet(ev_bits, EV_KEY)) {
            return false;
        }

        auto rel_bits = queryBitset(fd, EV_REL, REL_MAX);
        auto key_bits = queryBitset(fd, EV_KEY, KEY_MAX);
        if (rel_bits.empty() || key_bits.empty()) {
            return false;
        }

        return bitIsSet(rel_bits, REL_X) &&
               bitIsSet(rel_bits, REL_Y) &&
               bitIsSet(key_bits, BTN_LEFT);
    }

    std::vector<std::string> listEventDevicePaths()
    {
        std::vector<std::string> paths;
        const std::filesystem::path input_dir("/dev/input");
        if (!std::filesystem::exists(input_dir)) {
            return paths;
        }

        for (const auto& entry : std::filesystem::directory_iterator(input_dir)) {
            const auto filename = entry.path().filename().string();
            if (filename.rfind("event", 0) == 0) {
                paths.push_back(entry.path().string());
            }
        }

        std::sort(paths.begin(), paths.end());
        return paths;
    }

    std::optional<uint8_t> linuxKeyCodeToHidUsage(uint16_t code)
    {
        if (code >= KEY_A && code <= KEY_Z) {
            return static_cast<uint8_t>(0x04 + (code - KEY_A));
        }
        if (code >= KEY_1 && code <= KEY_9) {
            return static_cast<uint8_t>(0x1E + (code - KEY_1));
        }
        if (code == KEY_0) {
            return 0x27;
        }
        if (code >= KEY_F1 && code <= KEY_F12) {
            return static_cast<uint8_t>(0x3A + (code - KEY_F1));
        }
        if (code >= KEY_F13 && code <= KEY_F24) {
            return static_cast<uint8_t>(0x68 + (code - KEY_F13));
        }
        if (code >= KEY_KP1 && code <= KEY_KP9) {
            return static_cast<uint8_t>(0x59 + (code - KEY_KP1));
        }

        switch (code) {
        case KEY_ENTER: return 0x28;
        case KEY_ESC: return 0x29;
        case KEY_BACKSPACE: return 0x2A;
        case KEY_TAB: return 0x2B;
        case KEY_SPACE: return 0x2C;
        case KEY_MINUS: return 0x2D;
        case KEY_EQUAL: return 0x2E;
        case KEY_LEFTBRACE: return 0x2F;
        case KEY_RIGHTBRACE: return 0x30;
        case KEY_BACKSLASH: return 0x31;
        case KEY_SEMICOLON: return 0x33;
        case KEY_APOSTROPHE: return 0x34;
        case KEY_GRAVE: return 0x35;
        case KEY_COMMA: return 0x36;
        case KEY_DOT: return 0x37;
        case KEY_SLASH: return 0x38;
        case KEY_CAPSLOCK: return 0x39;
        case KEY_SYSRQ: return 0x46;
        case KEY_SCROLLLOCK: return 0x47;
        case KEY_PAUSE: return 0x48;
        case KEY_INSERT: return 0x49;
        case KEY_HOME: return 0x4A;
        case KEY_PAGEUP: return 0x4B;
        case KEY_DELETE: return 0x4C;
        case KEY_END: return 0x4D;
        case KEY_PAGEDOWN: return 0x4E;
        case KEY_RIGHT: return 0x4F;
        case KEY_LEFT: return 0x50;
        case KEY_DOWN: return 0x51;
        case KEY_UP: return 0x52;
        case KEY_NUMLOCK: return 0x53;
        case KEY_KPSLASH: return 0x54;
        case KEY_KPASTERISK: return 0x55;
        case KEY_KPMINUS: return 0x56;
        case KEY_KPPLUS: return 0x57;
        case KEY_KPENTER: return 0x58;
        case KEY_KP0: return 0x62;
        case KEY_KPDOT: return 0x63;
        case KEY_102ND: return 0x64;
        case KEY_COMPOSE: return 0x65;
        case KEY_POWER: return 0x66;
        case KEY_KPEQUAL: return 0x67;
        case KEY_MUTE: return 0x7F;
        case KEY_VOLUMEUP: return 0x80;
        case KEY_VOLUMEDOWN: return 0x81;
        default: return std::nullopt;
        }
    }

    std::optional<uint8_t> linuxKeyCodeToModifierBit(uint16_t code)
    {
        switch (code) {
        case KEY_LEFTCTRL: return 0;
        case KEY_LEFTSHIFT: return 1;
        case KEY_LEFTALT: return 2;
        case KEY_LEFTMETA: return 3;
        case KEY_RIGHTCTRL: return 4;
        case KEY_RIGHTSHIFT: return 5;
        case KEY_RIGHTALT: return 6;
        case KEY_RIGHTMETA: return 7;
        default: return std::nullopt;
        }
    }

    class EvdevInputReader {
    public:
        struct KeyboardState {
            uint8_t modifiers = 0;
            std::array<uint8_t, kKeyboardNkroBytes> nkro {};
        };

        struct MouseReport {
            uint8_t buttons = 0;
            int dx = 0;
            int dy = 0;
            int wheel = 0;
        };

        struct PollResult {
            std::vector<MouseReport> mouse_reports;
            bool mouse_buttons_changed = false;
            uint8_t mouse_buttons = 0;
            std::vector<KeyboardState> keyboard_states;
        };

        EvdevInputReader(std::string keyboard_path, std::string mouse_path, bool grab_on_start)
            : requested_keyboard_path_(sanitizeInputPath(std::move(keyboard_path)))
            , requested_mouse_path_(sanitizeInputPath(std::move(mouse_path)))
            , grab_enabled_(grab_on_start)
        {
            openOrDiscoverDevice(keyboard_, requested_keyboard_path_, true);
            openOrDiscoverDevice(mouse_, requested_mouse_path_, false);

            if (keyboard_.fd < 0 || mouse_.fd < 0) {
                throw std::runtime_error("Failed to open evdev keyboard/mouse devices");
            }

            if (grab_enabled_) {
                applyGrabState(true);
            }
        }

        ~EvdevInputReader()
        {
            if (keyboard_.fd >= 0) {
                if (grab_enabled_) {
                    setGrab(keyboard_.fd, false);
                }
                ::close(keyboard_.fd);
            }
            if (mouse_.fd >= 0) {
                if (grab_enabled_) {
                    setGrab(mouse_.fd, false);
                }
                ::close(mouse_.fd);
            }
        }

        PollResult PollReports()
        {
            PollResult result {};
            result.mouse_buttons = button_mask_;

            maybeReopenDevices();
            readAvailable(mouse_, false, result);
            readAvailable(keyboard_, true, result);

            result.mouse_buttons = button_mask_;
            return result;
        }

        [[nodiscard]] bool GrabEnabled() const
        {
            return grab_enabled_;
        }

    private:
        struct DeviceStream {
            int fd = -1;
            std::string active_path;
            std::vector<uint8_t> buffer;
        };

        void maybeReopenDevices()
        {
            if (keyboard_.fd >= 0 && mouse_.fd >= 0) {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < next_reopen_attempt_) {
                return;
            }
            next_reopen_attempt_ = now + std::chrono::seconds(1);

            if (keyboard_.fd < 0) {
                openOrDiscoverDevice(keyboard_, requested_keyboard_path_, true);
                if (keyboard_.fd >= 0 && grab_enabled_) {
                    setGrab(keyboard_.fd, true);
                }
            }
            if (mouse_.fd < 0) {
                openOrDiscoverDevice(mouse_, requested_mouse_path_, false);
                if (mouse_.fd >= 0 && grab_enabled_) {
                    setGrab(mouse_.fd, true);
                }
            }
        }

        void openOrDiscoverDevice(DeviceStream& device, const std::string& requested_path, bool want_keyboard)
        {
            const auto try_open = [&](const std::string& path) -> bool {
                const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) {
                    return false;
                }
                const bool matches = want_keyboard ? isKeyboardDevice(fd) : isMouseDevice(fd);
                if (!matches) {
                    ::close(fd);
                    return false;
                }

                device.fd = fd;
                device.active_path = path;
                device.buffer.clear();
                std::cout << "Opened " << (want_keyboard ? "keyboard" : "mouse")
                          << " evdev device: " << path << '\n';
                return true;
            };

            if (!requested_path.empty() && try_open(requested_path)) {
                return;
            }

            for (const auto& candidate : listEventDevicePaths()) {
                if (try_open(candidate)) {
                    return;
                }
            }

            std::cerr << "Failed to find " << (want_keyboard ? "keyboard" : "mouse")
                      << " evdev device (requested path: " << requested_path << ")\n";
        }

        static bool setGrab(int fd, bool enabled)
        {
            if (fd < 0) {
                return false;
            }
            if (::ioctl(fd, EVIOCGRAB, enabled ? 1 : 0) < 0) {
                std::cerr << "EVIOCGRAB(" << (enabled ? "1" : "0") << ") failed: "
                          << std::strerror(errno) << '\n';
                return false;
            }
            return true;
        }

        void applyGrabState(bool enabled)
        {
            const bool keyboard_ok = setGrab(keyboard_.fd, enabled);
            const bool mouse_ok = setGrab(mouse_.fd, enabled);
            if (keyboard_ok && mouse_ok) {
                grab_enabled_ = enabled;
                std::cout << "Input grab " << (enabled ? "enabled" : "disabled") << '\n';
            } else {
                std::cerr << "Input grab " << (enabled ? "enable" : "disable")
                          << " failed on one or more devices\n";
            }
        }

        void toggleGrab()
        {
            applyGrabState(!grab_enabled_);
        }

        void closeDevice(DeviceStream& device, const char* label)
        {
            if (device.fd >= 0) {
                ::close(device.fd);
            }
            std::cerr << label << " evdev device disconnected: " << device.active_path << '\n';
            device.fd = -1;
            device.active_path.clear();
            device.buffer.clear();
        }

        void readAvailable(DeviceStream& device, bool keyboard, PollResult& result)
        {
            if (device.fd < 0) {
                return;
            }

            uint8_t temp[sizeof(input_event) * 32];
            while (true) {
                const ssize_t n = ::read(device.fd, temp, sizeof(temp));
                if (n > 0) {
                    device.buffer.insert(device.buffer.end(), temp, temp + n);
                    processBufferedEvents(device, keyboard, result);
                    continue;
                }

                if (n == 0) {
                    closeDevice(device, keyboard ? "Keyboard" : "Mouse");
                    return;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }

                std::cerr << "read failed for " << (keyboard ? "keyboard" : "mouse")
                          << " device: " << std::strerror(errno) << '\n';
                closeDevice(device, keyboard ? "Keyboard" : "Mouse");
                return;
            }
        }

        void processBufferedEvents(DeviceStream& device, bool keyboard, PollResult& result)
        {
            const std::size_t event_size = sizeof(input_event);
            std::size_t offset = 0;
            while (device.buffer.size() - offset >= event_size) {
                input_event ev {};
                std::memcpy(&ev, device.buffer.data() + offset, event_size);
                offset += event_size;

                if (keyboard) {
                    handleKeyboardEvent(ev, result);
                } else {
                    handleMouseEvent(ev, result);
                }
            }

            if (offset > 0) {
                device.buffer.erase(device.buffer.begin(), device.buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }

        bool updateModifierBit(uint8_t bit, bool pressed)
        {
            const uint8_t mask = static_cast<uint8_t>(1u << bit);
            const bool was_set = (keyboard_modifiers_ & mask) != 0;
            if (pressed) {
                keyboard_modifiers_ = static_cast<uint8_t>(keyboard_modifiers_ | mask);
            } else {
                keyboard_modifiers_ = static_cast<uint8_t>(keyboard_modifiers_ & ~mask);
            }
            return was_set != pressed;
        }

        void emitKeyboardState(PollResult& result)
        {
            result.keyboard_states.push_back({keyboard_modifiers_, keyboard_nkro_});
        }

        void handleMouseEvent(const input_event& ev, PollResult& result)
        {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    mouse_dx_ += ev.value;
                    mouse_dirty_ = true;
                } else if (ev.code == REL_Y) {
                    mouse_dy_ += ev.value;
                    mouse_dirty_ = true;
                } else if (ev.code == REL_WHEEL) {
                    mouse_wheel_ += ev.value;
                    mouse_dirty_ = true;
                } else if (ev.code == REL_HWHEEL) {
                    if (!warned_horizontal_scroll_) {
                        std::cout << "Ignoring horizontal wheel events (descriptor exposes vertical wheel only)\n";
                        warned_horizontal_scroll_ = true;
                    }
                }
                return;
            }

            if (ev.type == EV_KEY) {
                uint8_t mask = 0;
                if (ev.code == BTN_LEFT) {
                    mask = 0x01;
                } else if (ev.code == BTN_RIGHT) {
                    mask = 0x02;
                } else if (ev.code == BTN_MIDDLE) {
                    mask = 0x04;
                }

                if (mask != 0) {
                    const bool pressed = ev.value != 0;
                    const uint8_t old = button_mask_;
                    if (pressed) {
                        button_mask_ = static_cast<uint8_t>(button_mask_ | mask);
                    } else {
                        button_mask_ = static_cast<uint8_t>(button_mask_ & ~mask);
                    }
                    if (old != button_mask_) {
                        result.mouse_buttons_changed = true;
                        result.mouse_buttons = button_mask_;
                        mouse_dirty_ = true;
                    }
                }
                return;
            }

            if (ev.type == EV_SYN && ev.code == SYN_REPORT && mouse_dirty_) {
                result.mouse_reports.push_back({button_mask_, mouse_dx_, mouse_dy_, mouse_wheel_});
                mouse_dx_ = 0;
                mouse_dy_ = 0;
                mouse_wheel_ = 0;
                mouse_dirty_ = false;
            }
        }

        void handleKeyboardEvent(const input_event& ev, PollResult& result)
        {
            if (ev.type != EV_KEY) {
                return;
            }

            const bool pressed = ev.value != 0;
            const bool repeat = ev.value == 2;

            if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
                const bool is_left = ev.code == KEY_LEFTCTRL;
                if (is_left) {
                    left_ctrl_physical_ = pressed;
                } else {
                    right_ctrl_physical_ = pressed;
                }

                const bool both_pressed = left_ctrl_physical_ && right_ctrl_physical_;
                if (both_pressed && !combo_latched_) {
                    combo_latched_ = true;
                    toggleGrab();
                    bool changed = false;
                    changed = updateModifierBit(0, false) || changed;
                    changed = updateModifierBit(4, false) || changed;
                    if (changed) {
                        emitKeyboardState(result);
                    }
                    return;
                }

                if (combo_latched_) {
                    // Re-arm only after both keys are released to avoid double-toggles
                    // from transient/synthetic key state changes.
                    if (!left_ctrl_physical_ && !right_ctrl_physical_) {
                        combo_latched_ = false;
                    }
                    bool changed = false;
                    changed = updateModifierBit(0, false) || changed;
                    changed = updateModifierBit(4, false) || changed;
                    if (changed) {
                        emitKeyboardState(result);
                    }
                    return;
                }
            }

            if (const auto mod_bit = linuxKeyCodeToModifierBit(ev.code); mod_bit.has_value()) {
                if (!repeat && updateModifierBit(*mod_bit, pressed)) {
                    emitKeyboardState(result);
                }
                return;
            }

            if (repeat) {
                return;
            }

            const auto usage = linuxKeyCodeToHidUsage(ev.code);
            if (!usage.has_value()) {
                return;
            }

            if (setKeyboardUsageBit(keyboard_nkro_, *usage, pressed)) {
                emitKeyboardState(result);
            }
        }

        std::string requested_keyboard_path_;
        std::string requested_mouse_path_;
        DeviceStream keyboard_;
        DeviceStream mouse_;
        bool grab_enabled_ = true;
        std::chrono::steady_clock::time_point next_reopen_attempt_ {};

        uint8_t button_mask_ = 0;
        uint8_t keyboard_modifiers_ = 0;
        std::array<uint8_t, kKeyboardNkroBytes> keyboard_nkro_ {};

        int mouse_dx_ = 0;
        int mouse_dy_ = 0;
        int mouse_wheel_ = 0;
        bool mouse_dirty_ = false;
        bool warned_horizontal_scroll_ = false;

        bool left_ctrl_physical_ = false;
        bool right_ctrl_physical_ = false;
        bool combo_latched_ = false;
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
        const std::string keyboard_event_path = sanitizeInputPath(
            readStringFromEnv(kEnvKeyboardEventPath, kDefaultKeyboardEventPath));
        const std::string mouse_event_path = sanitizeInputPath(
            readStringFromEnv(kEnvMouseEventPath, kDefaultMouseEventPath));
        const bool grab_on_start = readBoolFromEnv(kEnvGrabOnStart, false);
        std::cout << "Reading evdev input from keyboard='" << keyboard_event_path
                  << "' mouse='" << mouse_event_path
                  << "' grab_on_start=" << (grab_on_start ? "true" : "false") << '\n';
        EvdevInputReader input_reader(keyboard_event_path, mouse_event_path, grab_on_start);
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
            const int scaled_dx = scaleDeltaWithMultiplier(coalesced_dx, delta_multiplier);
            const int scaled_dy = scaleDeltaWithMultiplier(coalesced_dy, delta_multiplier);
            const int scaled_wheel = scaleDeltaWithMultiplier(coalesced_wheel, wheel_multiplier);
            const int8_t dx = clampToInt8(scaled_dx);
            const int8_t dy = clampToInt8(scaled_dy);
            const int8_t wheel = clampToInt8(invert_scroll_direction ? -scaled_wheel : scaled_wheel);

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
