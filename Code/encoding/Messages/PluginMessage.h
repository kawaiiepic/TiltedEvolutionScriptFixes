#pragma once

#include "Message.h"
#include <string_view>

struct PluginMessagePayload
{
    static constexpr size_t MaxChannelLength = 64;
    static constexpr size_t MaxPayloadSize = 4096;
    static constexpr uint32_t Nearby = UINT32_MAX;
    static bool IsValidChannel(std::string_view aChannel) noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer&) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader&) noexcept;

    TiltedPhoques::String Channel;
    TiltedPhoques::Vector<uint8_t> Bytes;
};

struct RequestPluginMessage final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestPluginMessage;
    RequestPluginMessage() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;

    uint32_t Target{};
    PluginMessagePayload Payload;
};

struct NotifyPluginMessage final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPluginMessage;
    NotifyPluginMessage() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;

    uint32_t Sender{};
    PluginMessagePayload Payload;
};
