#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace Campaign
{
struct PluginFormKey
{
    std::string Plugin;
    bool Lite{};
    uint32_t BaseId{};

    auto operator<=>(const PluginFormKey&) const = default;

    bool IsValid() const noexcept
    {
        return !Plugin.empty() && Plugin.find_first_of("\r\n") == std::string::npos &&
               Plugin.find('\0') == std::string::npos && BaseId != 0 && BaseId <= (Lite ? 0xFFFu : 0xFFFFFFu);
    }
};
}
