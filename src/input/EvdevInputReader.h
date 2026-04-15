#pragma once

#include "src/hid/HidReports.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <linux/input.h>

namespace rodent::input {

// Reads keyboard and mouse input events from Linux evdev devices and converts them to HID reports
class EvdevInputReader {
public:
    // Represents the current state of keyboard modifiers and pressed keys
    struct KeyboardState {
        uint8_t modifiers = 0;
        std::array<uint8_t, hid::kKeyboardNkroBytes> nkro {};
    };

    // Represents a single mouse motion/button event with relative deltas
    struct MouseReport {
        uint8_t buttons = 0;
        int dx = 0;
        int dy = 0;
        int wheel = 0;
    };

    // Aggregated input events from a single poll cycle
    struct PollResult {
        std::vector<MouseReport> mouse_reports;
        bool mouse_buttons_changed = false;
        uint8_t mouse_buttons = 0;
        std::vector<KeyboardState> keyboard_states;
    };

    // Initialize reader with paths to keyboard and mouse evdev devices
    // grab_on_start: if true, grab input devices to prevent system from receiving input
    EvdevInputReader(std::string keyboard_path, std::string mouse_path, bool grab_on_start);
    ~EvdevInputReader();

    // Poll for input events and return collected reports
    PollResult PollReports();
    
    // Check if input grab is currently enabled
    [[nodiscard]] bool GrabEnabled() const;

private:
    // Represents an open evdev device and its event buffer
    struct DeviceStream {
        int fd = -1;
        std::string active_path;
        std::vector<uint8_t> buffer;
    };

    // Attempt to reopen any closed devices (with rate limiting)
    void maybeReopenDevices();
    
    // Open or discover an evdev device matching the requested path or type
    void openOrDiscoverDevice(DeviceStream& device, const std::string& requested_path, bool want_keyboard);
    
    // Enable or disable exclusive input grabbing on a file descriptor
    static bool setGrab(int fd, bool enabled);
    
    // Apply grab state to all open input devices
    void applyGrabState(bool enabled);
    
    // Toggle grab state between enabled and disabled
    void toggleGrab();
    
    // Close an open device and log the disconnection
    void closeDevice(DeviceStream& device, const char* label);
    
    // Read available input data from device and add to buffer
    void readAvailable(DeviceStream& device, bool keyboard, PollResult& result);
    
    // Process buffered input events and populate PollResult
    void processBufferedEvents(DeviceStream& device, bool keyboard, PollResult& result);
    
    // Update a modifier bit based on key press state
    bool updateModifierBit(uint8_t bit, bool pressed);
    
    // Emit current keyboard state if changed
    void emitKeyboardState(PollResult& result);
    
    // Process mouse input event (buttons, motion, wheel)
    void handleMouseEvent(const input_event& ev, PollResult& result);
    
    // Process keyboard input event (modifiers, keys)
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
    bool escape_physical_ = false;
    bool combo_latched_ = false;
};

}  // namespace rodent::input
