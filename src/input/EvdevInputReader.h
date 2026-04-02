#pragma once

#include "src/hid/HidReports.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <linux/input.h>

namespace rodent::input {

class EvdevInputReader {
public:
    struct KeyboardState {
        uint8_t modifiers = 0;
        std::array<uint8_t, hid::kKeyboardNkroBytes> nkro {};
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

    EvdevInputReader(std::string keyboard_path, std::string mouse_path, bool grab_on_start);
    ~EvdevInputReader();

    PollResult PollReports();
    [[nodiscard]] bool GrabEnabled() const;

private:
    struct DeviceStream {
        int fd = -1;
        std::string active_path;
        std::vector<uint8_t> buffer;
    };

    void maybeReopenDevices();
    void openOrDiscoverDevice(DeviceStream& device, const std::string& requested_path, bool want_keyboard);
    static bool setGrab(int fd, bool enabled);
    void applyGrabState(bool enabled);
    void toggleGrab();
    void closeDevice(DeviceStream& device, const char* label);
    void readAvailable(DeviceStream& device, bool keyboard, PollResult& result);
    void processBufferedEvents(DeviceStream& device, bool keyboard, PollResult& result);
    bool updateModifierBit(uint8_t bit, bool pressed);
    void emitKeyboardState(PollResult& result);
    void handleMouseEvent(const input_event& ev, PollResult& result);
    void handleKeyboardEvent(const input_event& ev, PollResult& result);

    std::string requested_keyboard_path_;
    std::string requested_mouse_path_;
    DeviceStream keyboard_;
    DeviceStream mouse_;
    bool grab_enabled_ = true;
    std::chrono::steady_clock::time_point next_reopen_attempt_ {};

    uint8_t button_mask_ = 0;
    uint8_t keyboard_modifiers_ = 0;
    std::array<uint8_t, hid::kKeyboardNkroBytes> keyboard_nkro_ {};

    int mouse_dx_ = 0;
    int mouse_dy_ = 0;
    int mouse_wheel_ = 0;
    int mouse_wheel_hi_res_accum_ = 0;
    bool mouse_dirty_ = false;
    bool warned_horizontal_scroll_ = false;

    bool left_ctrl_physical_ = false;
    bool right_ctrl_physical_ = false;
    bool combo_latched_ = false;
};

}  // namespace rodent::input
