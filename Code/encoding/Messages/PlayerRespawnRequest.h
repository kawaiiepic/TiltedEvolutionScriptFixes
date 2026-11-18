#pragma once

#include "Message.h"

struct PlayerRespawnRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPlayerRespawnRequest;

    PlayerRespawnRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    enum ActionCode : uint8_t
    {
        Downed,
        Respawn,
        Revive,
        Recovered
    };

    uint32_t DownId{};
    uint8_t Action{Downed};
    uint32_t TargetId{};

    bool operator==(const PlayerRespawnRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && DownId == acRhs.DownId && Action == acRhs.Action && TargetId == acRhs.TargetId; }
};
