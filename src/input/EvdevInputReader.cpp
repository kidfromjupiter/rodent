#include "EvdevInputReader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef REL_WHEEL_HI_RES
#define REL_WHEEL_HI_RES 0x0b
#endif

#ifndef REL_HWHEEL_HI_RES
#define REL_HWHEEL_HI_RES 0x0c
#endif

namespace rodent::input {
namespace {

constexpr uint8_t kKeyboardUsageMin = 0x04;
constexpr uint8_t kKeyboardUsageMax = 0xE7;
constexpr int kWheelHiResUnitsPerDetent = 120;

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

bool isKeyboardUsageInRange(uint8_t usage)
{
    return usage >= kKeyboardUsageMin && usage <= kKeyboardUsageMax;
}

bool setKeyboardUsageBit(std::array<uint8_t, hid::kKeyboardNkroBytes>& bitmap, uint8_t usage, bool pressed)
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

std::optional<uint8_t> linuxKeyCodeToHidUsage(uint16_t code)
{
    switch (code) {
    case KEY_A: return 0x04;
    case KEY_B: return 0x05;
    case KEY_C: return 0x06;
    case KEY_D: return 0x07;
    case KEY_E: return 0x08;
    case KEY_F: return 0x09;
    case KEY_G: return 0x0A;
    case KEY_H: return 0x0B;
    case KEY_I: return 0x0C;
    case KEY_J: return 0x0D;
    case KEY_K: return 0x0E;
    case KEY_L: return 0x0F;
    case KEY_M: return 0x10;
    case KEY_N: return 0x11;
    case KEY_O: return 0x12;
    case KEY_P: return 0x13;
    case KEY_Q: return 0x14;
    case KEY_R: return 0x15;
    case KEY_S: return 0x16;
    case KEY_T: return 0x17;
    case KEY_U: return 0x18;
    case KEY_V: return 0x19;
    case KEY_W: return 0x1A;
    case KEY_X: return 0x1B;
    case KEY_Y: return 0x1C;
    case KEY_Z: return 0x1D;
    case KEY_1: return 0x1E;
    case KEY_2: return 0x1F;
    case KEY_3: return 0x20;
    case KEY_4: return 0x21;
    case KEY_5: return 0x22;
    case KEY_6: return 0x23;
    case KEY_7: return 0x24;
    case KEY_8: return 0x25;
    case KEY_9: return 0x26;
    case KEY_0: return 0x27;
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
    case KEY_F1: return 0x3A;
    case KEY_F2: return 0x3B;
    case KEY_F3: return 0x3C;
    case KEY_F4: return 0x3D;
    case KEY_F5: return 0x3E;
    case KEY_F6: return 0x3F;
    case KEY_F7: return 0x40;
    case KEY_F8: return 0x41;
    case KEY_F9: return 0x42;
    case KEY_F10: return 0x43;
    case KEY_F11: return 0x44;
    case KEY_F12: return 0x45;
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
    case KEY_KP1: return 0x59;
    case KEY_KP2: return 0x5A;
    case KEY_KP3: return 0x5B;
    case KEY_KP4: return 0x5C;
    case KEY_KP5: return 0x5D;
    case KEY_KP6: return 0x5E;
    case KEY_KP7: return 0x5F;
    case KEY_KP8: return 0x60;
    case KEY_KP9: return 0x61;
    case KEY_KP0: return 0x62;
    case KEY_KPDOT: return 0x63;
    case KEY_102ND: return 0x64;
    case KEY_COMPOSE: return 0x65;
    case KEY_POWER: return 0x66;
    case KEY_KPEQUAL: return 0x67;
    case KEY_F13: return 0x68;
    case KEY_F14: return 0x69;
    case KEY_F15: return 0x6A;
    case KEY_F16: return 0x6B;
    case KEY_F17: return 0x6C;
    case KEY_F18: return 0x6D;
    case KEY_F19: return 0x6E;
    case KEY_F20: return 0x6F;
    case KEY_F21: return 0x70;
    case KEY_F22: return 0x71;
    case KEY_F23: return 0x72;
    case KEY_F24: return 0x73;
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

}  // namespace

EvdevInputReader::EvdevInputReader(std::string keyboard_path, std::string mouse_path, bool grab_on_start)
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

EvdevInputReader::~EvdevInputReader()
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

EvdevInputReader::PollResult EvdevInputReader::PollReports()
{
    PollResult result {};
    result.mouse_buttons = button_mask_;

    maybeReopenDevices();
    readAvailable(mouse_, false, result);
    readAvailable(keyboard_, true, result);

    result.mouse_buttons = button_mask_;
    return result;
}

bool EvdevInputReader::GrabEnabled() const
{
    return grab_enabled_;
}

void EvdevInputReader::maybeReopenDevices()
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

void EvdevInputReader::openOrDiscoverDevice(DeviceStream& device, const std::string& requested_path, bool want_keyboard)
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

bool EvdevInputReader::setGrab(int fd, bool enabled)
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

void EvdevInputReader::applyGrabState(bool enabled)
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

void EvdevInputReader::toggleGrab()
{
    applyGrabState(!grab_enabled_);
}

void EvdevInputReader::closeDevice(DeviceStream& device, const char* label)
{
    if (device.fd >= 0) {
        ::close(device.fd);
    }
    std::cerr << label << " evdev device disconnected: " << device.active_path << '\n';
    device.fd = -1;
    device.active_path.clear();
    device.buffer.clear();
}

void EvdevInputReader::readAvailable(DeviceStream& device, bool keyboard, PollResult& result)
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

void EvdevInputReader::processBufferedEvents(DeviceStream& device, bool keyboard, PollResult& result)
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

bool EvdevInputReader::updateModifierBit(uint8_t bit, bool pressed)
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

void EvdevInputReader::emitKeyboardState(PollResult& result)
{
    result.keyboard_states.push_back({keyboard_modifiers_, keyboard_nkro_});
}

void EvdevInputReader::handleMouseEvent(const input_event& ev, PollResult& result)
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
        } else if (ev.code == REL_WHEEL_HI_RES) {
            mouse_wheel_hi_res_accum_ += ev.value;
        } else if (ev.code == REL_HWHEEL) {
            if (!warned_horizontal_scroll_) {
                std::cout << "Ignoring horizontal wheel events (descriptor exposes vertical wheel only)\n";
                warned_horizontal_scroll_ = true;
            }
        } else if (ev.code == REL_HWHEEL_HI_RES) {
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

    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        const int hi_res_steps = mouse_wheel_hi_res_accum_ / kWheelHiResUnitsPerDetent;
        if (hi_res_steps != 0) {
            mouse_wheel_ += hi_res_steps;
            mouse_wheel_hi_res_accum_ -= hi_res_steps * kWheelHiResUnitsPerDetent;
            mouse_dirty_ = true;
        }

        if (mouse_dirty_) {
            result.mouse_reports.push_back({button_mask_, mouse_dx_, mouse_dy_, mouse_wheel_});
            mouse_dx_ = 0;
            mouse_dy_ = 0;
            mouse_wheel_ = 0;
            mouse_dirty_ = false;
        }
    }
}

void EvdevInputReader::handleKeyboardEvent(const input_event& ev, PollResult& result)
{
    if (ev.type != EV_KEY) {
        return;
    }

    const bool pressed = ev.value != 0;
    const bool repeat = ev.value == 2;

    if (ev.code == KEY_LEFTCTRL || ev.code == KEY_ESC) {
        if (ev.code == KEY_LEFTCTRL) {
            left_ctrl_physical_ = pressed;
        } else {
            escape_physical_ = pressed;
        }

        const bool both_pressed = left_ctrl_physical_ && escape_physical_;
        const bool both_released = !left_ctrl_physical_ && !escape_physical_;
        if (both_pressed && !combo_latched_) {
            combo_latched_ = true;
            bool changed = false;
            changed = updateModifierBit(0, false) || changed;
            changed = updateModifierBit(4, false) || changed;
            if (changed) {
                emitKeyboardState(result);
            }
            return;
        }

        if (combo_latched_) {
            if (both_released) {
                toggleGrab();
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

}  // namespace rodent::input
