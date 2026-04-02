#include "RuntimeConfig.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <bluetooth/bluetooth.h>

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

uint16_t ReadControllerIndexFromEnv(const char* env_name, uint16_t default_value)
{
    const char* raw = std::getenv(env_name);
    if (raw == nullptr || raw[0] == '\0') {
        return default_value;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed > std::numeric_limits<uint16_t>::max()) {
        std::cerr << "Invalid " << env_name << "='" << raw << "', defaulting to " << default_value << '\n';
        return default_value;
    }

    return static_cast<uint16_t>(parsed);
}

uint8_t ReadU8FromEnv(const char* env_name, uint8_t default_value)
{
    const char* raw = std::getenv(env_name);
    if (raw == nullptr || raw[0] == '\0') {
        return default_value;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(raw, &end, 0);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0') || parsed > std::numeric_limits<uint8_t>::max()) {
        std::cerr << "Invalid " << env_name << "='" << raw << "', defaulting to " << static_cast<int>(default_value) << '\n';
        return default_value;
    }

    return static_cast<uint8_t>(parsed);
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

std::optional<bluetooth::DirectedTargetConfig> ReadDirectedTargetConfig()
{
    const char* raw_addr = std::getenv(kEnvLeTargetAddress);
    if (raw_addr == nullptr || raw_addr[0] == '\0') {
        return std::nullopt;
    }

    bluetooth::DirectedTargetConfig cfg;
    if (::str2ba(raw_addr, &cfg.addr) != 0) {
        throw std::runtime_error(std::string("Invalid BLE target address in ") + kEnvLeTargetAddress + ": " + raw_addr);
    }
    cfg.addr_type = ReadU8FromEnv(kEnvLeTargetAddrType, cfg.addr_type);
    cfg.action = ReadU8FromEnv(kEnvLeTargetAction, cfg.action);
    return cfg;
}

}  // namespace rodent::runtime
