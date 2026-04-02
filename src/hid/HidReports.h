#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rodent::hid {

inline constexpr std::size_t kKeyboardNkroBytes = 29;

int8_t ClampToInt8(int value);

std::vector<uint8_t> BuildKeyboardInputReport(
    uint8_t modifiers,
    const std::array<uint8_t, kKeyboardNkroBytes>& nkro);

const std::vector<uint8_t>& CompositeHidReportMap();

}  // namespace rodent::hid
