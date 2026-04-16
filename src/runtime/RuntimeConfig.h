#pragma once

#include <cstdint>
#include <string>

namespace rodent::runtime {

inline constexpr const char* kDefaultKeyboardEventPath = "/dev/input/event3";
inline constexpr const char* kDefaultMouseEventPath = "/dev/input/event5";
inline constexpr const char* kEnvDpiMultiplier = "RODENT_DPI_MULTIPLIER";
inline constexpr const char* kEnvSensitivityMultiplier = "RODENT_SENSITIVITY_MULTIPLIER";
inline constexpr const char* kEnvWheelMultiplier = "RODENT_WHEEL_MULTIPLIER";
inline constexpr const char* kEnvInvertScrollDirection = "RODENT_INVERT_SCROLL_DIRECTION";
inline constexpr const char* kEnvKeyboardEventPath = "RODENT_EVDEV_KEYBOARD_PATH";
inline constexpr const char* kEnvMouseEventPath = "RODENT_EVDEV_MOUSE_PATH";
inline constexpr const char* kEnvGrabOnStart = "RODENT_GRAB_ON_START";
inline constexpr const char* kEnvClipboardWatchSeat = "RODENT_CLIPBOARD_WATCH_SEAT";
inline constexpr const char* kEnvClipboardWatchPrimary = "RODENT_CLIPBOARD_WATCH_PRIMARY";

float ReadMultiplierFromEnv(const char* env_name, float default_value);
bool ReadBoolFromEnv(const char* env_name, bool default_value);
std::string ReadStringFromEnv(const char* env_name, const char* default_value);
std::string SanitizeInputPath(std::string path);
int ScaleDeltaWithMultiplier(int delta, float multiplier);

}  // namespace rodent::runtime
