#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rodent::hid {

// Number of bytes used for keyboard NKRO (N-key rollover) state representation
inline constexpr std::size_t kKeyboardNkroBytes = 29;

// Clamps a signed integer to the range [-127, 127] for int8_t representation
int8_t ClampToInt8(int value);

// Builds a keyboard HID input report (report ID 0x02)
// Returns: [modifiers, reserved, nkro_bytes...]
std::vector<uint8_t> BuildKeyboardInputReport(
    uint8_t modifiers,
    const std::array<uint8_t, kKeyboardNkroBytes>& nkro);

// Builds a touchpad HID input report (report ID 0x03)
// Returns: [0x03, x_high, x_low, y_high, y_low] - absolute coordinates as 16-bit big-endian values
std::vector<uint8_t> BuildTouchpadInputReport(uint16_t x, uint16_t y);

// Returns the composite HID report descriptor containing:
// - Mouse collection (report ID 0x01): relative movement and buttons
// - Keyboard collection (report ID 0x02): modifiers and key states
// - Touchpad collection (report ID 0x03): absolute X/Y coordinates
const std::vector<uint8_t>& CompositeHidReportMap();

}  // namespace rodent::hid
