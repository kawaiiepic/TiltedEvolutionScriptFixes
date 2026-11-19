#pragma once

#include <cstddef>
#include <cstdint>

namespace SkyrimTogether
{
inline constexpr uint32_t PluginMessagingVersion = 1;
inline constexpr uint32_t Broadcast = 0;
inline constexpr uint32_t Nearby = UINT32_MAX;
inline constexpr size_t MaxChannelLength = 64;
inline constexpr size_t MaxPayloadSize = 4096;

enum class PluginMessageResult : uint32_t
{
    Ok, InvalidArgument, UnsupportedVersion, NotConnected, WrongThread,
    ChannelInUse, ChannelNotRegistered, LimitReached, SendFailed
};

// Player IDs are server-session IDs. The payload is valid only during the
// callback, which runs on the game thread. A callback must not throw.
using PluginMessageCallback = void (*)(uint32_t aSender, const void* apData, size_t aSize, void* apContext);

struct PluginMessagingInterface
{
    uint32_t Version;
    PluginMessageResult (*Register)(const char* apChannel, PluginMessageCallback aCallback, void* apContext);
    PluginMessageResult (*Unregister)(const char* apChannel, PluginMessageCallback aCallback);
    PluginMessageResult (*Send)(const char* apChannel, uint32_t aTarget, const void* apData, size_t aSize);
    uint32_t (*LocalPlayerId)();
    uint32_t (*ResolvePlayerFormId)(uint32_t aPlayerId);
};

using QueryPluginMessaging = PluginMessageResult (*)(uint32_t aVersion, const PluginMessagingInterface** appInterface);
inline constexpr char PluginMessagingExport[] = "ST_QueryPluginMessaging";
}
