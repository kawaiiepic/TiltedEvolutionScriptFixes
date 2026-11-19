#pragma once

#include <string_view>

namespace PluginMessagingProtocol
{
inline constexpr std::string_view Prefix = "STRPM|";
inline constexpr std::string_view Version2Prefix = "STRPM|v2|";

inline bool IsEnvelope(std::string_view aMessage) noexcept
{
    return aMessage.starts_with(Prefix);
}
}
