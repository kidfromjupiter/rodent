#include "RuntimeConfig.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace rodent::runtime {

float ReadMultiplierFromEnv(const char* env_name, float default_value)
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

bool ReadBoolFromEnv(const char* env_name, bool default_value)
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

std::string ReadStringFromEnv(const char* env_name, const char* default_value)
{
    const char* raw = std::getenv(env_name);
    if (raw == nullptr || raw[0] == '\0') {
        return default_value;
    }
    return raw;
}

std::string SanitizeInputPath(std::string path)
{
    while (!path.empty() && path.back() == '@') {
        path.pop_back();
    }
    return path;
}

int ScaleDeltaWithMultiplier(int delta, float multiplier)
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

}  // namespace rodent::runtime
