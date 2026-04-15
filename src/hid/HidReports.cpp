#include "HidReports.h"

namespace rodent::hid {

// Clamps a value to the signed 8-bit range to prevent overflow
int8_t ClampToInt8(int value)
{
    if (value < -127) {
        return -127;
    }
    if (value > 127) {
        return 127;
    }
    return static_cast<int8_t>(value);
}

// Builds a keyboard HID input report with modifiers and key states
std::vector<uint8_t> BuildKeyboardInputReport(
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

// Builds a touchpad HID input report with absolute X/Y coordinates
// Format: report_id (0x03), x_high, x_low, y_high, y_low
std::vector<uint8_t> BuildTouchpadInputReport(uint16_t x, uint16_t y)
{
    std::vector<uint8_t> report;
    report.reserve(5);
    report.push_back(0x03);
    report.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    report.push_back(static_cast<uint8_t>(x & 0xFF));
    report.push_back(static_cast<uint8_t>((y >> 8) & 0xFF));
    report.push_back(static_cast<uint8_t>(y & 0xFF));
    return report;
}

// Returns the composite HID report descriptor for the device
// Includes three collections:
// - Mouse (report ID 0x01): buttons 1-5, X/Y/wheel relative motion
// - Keyboard (report ID 0x02): 8 modifier bits, 232 key states (NKRO)
// - Touchpad (report ID 0x03): absolute X/Y coordinates (16-bit each)
const std::vector<uint8_t>& CompositeHidReportMap()
{
    static const std::vector<uint8_t> kCompositeHidReportMap {
        // Mouse collection (report ID 0x01)
        0x05, 0x01,        // Usage page: Generic Desktop Controls
        0x09, 0x02,        // Usage: Mouse
        0xA1, 0x01,        // Collection: Application
        0x85, 0x01,        // Report ID: 0x01
        0x09, 0x01,        // Usage: Pointer
        0xA1, 0x00,        // Collection: Physical

        0x05, 0x09,        // Usage page: Button
        0x19, 0x01,        // Usage minimum: Button 1
        0x29, 0x05,        // Usage maximum: Button 5
        0x15, 0x00,        // Logical minimum: 0
        0x25, 0x01,        // Logical maximum: 1
        0x95, 0x05,        // Report count: 5
        0x75, 0x01,        // Report size: 1 bit
        0x81, 0x02,        // Input: Data, Variable, Absolute

        0x95, 0x01,        // Report count: 1
        0x75, 0x03,        // Report size: 3 bits (padding)
        0x81, 0x03,        // Input: Constant, Variable, Absolute

        0x05, 0x01,        // Usage page: Generic Desktop Controls
        0x09, 0x30,        // Usage: X
        0x09, 0x31,        // Usage: Y
        0x09, 0x38,        // Usage: Wheel
        0x15, 0x81,        // Logical minimum: -127
        0x25, 0x7F,        // Logical maximum: 127
        0x75, 0x08,        // Report size: 8 bits
        0x95, 0x03,        // Report count: 3
        0x81, 0x06,        // Input: Data, Variable, Relative

        0xC0,              // End collection
        0xC0,              // End collection

        // Keyboard collection (report ID 0x02)
        0x05, 0x01,        // Usage page: Generic Desktop Controls
        0x09, 0x06,        // Usage: Keyboard
        0xA1, 0x01,        // Collection: Application
        0x85, 0x02,        // Report ID: 0x02
        0x05, 0x07,        // Usage page: Keyboard/Keypad
        0x19, 0xE0,        // Usage minimum: Left Control
        0x29, 0xE7,        // Usage maximum: Right GUI
        0x15, 0x00,        // Logical minimum: 0
        0x25, 0x01,        // Logical maximum: 1
        0x75, 0x01,        // Report size: 1 bit
        0x95, 0x08,        // Report count: 8
        0x81, 0x02,        // Input: Data, Variable, Absolute
        0x75, 0x08,        // Report size: 8 bits
        0x95, 0x01,        // Report count: 1
        0x81, 0x01,        // Input: Constant, Variable, Absolute
        0x05, 0x07,        // Usage page: Keyboard/Keypad
        0x19, 0x04,        // Usage minimum: Error Roll Over
        0x29, 0xE7,        // Usage maximum: Right GUI
        0x15, 0x00,        // Logical minimum: 0
        0x25, 0x01,        // Logical maximum: 1
        0x75, 0x01,        // Report size: 1 bit
        0x95, 0xE4,        // Report count: 228
        0x81, 0x02,        // Input: Data, Variable, Absolute
        0x75, 0x01,        // Report size: 1 bit
        0x95, 0x04,        // Report count: 4
        0x81, 0x01,        // Input: Constant, Variable, Absolute
        0xC0,              // End collection

        // Touchpad collection (report ID 0x03)
        0x05, 0x01,        // Usage page: Generic Desktop Controls
        0x09, 0x04,        // Usage: Digitizer (Touchpad)
        0xA1, 0x01,        // Collection: Application
        0x85, 0x03,        // Report ID: 0x03
        0x05, 0x01,        // Usage page: Generic Desktop Controls
        0x09, 0x30,        // Usage: X
        0x09, 0x31,        // Usage: Y
        0x15, 0x00,        // Logical minimum: 0
        0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical maximum: 65535
        0x75, 0x10,        // Report size: 16 bits
        0x95, 0x02,        // Report count: 2 (X and Y)
        0x81, 0x02,        // Input: Data, Variable, Absolute
        0xC0               // End collection
    };
    return kCompositeHidReportMap;
}

}  // namespace rodent::hid
