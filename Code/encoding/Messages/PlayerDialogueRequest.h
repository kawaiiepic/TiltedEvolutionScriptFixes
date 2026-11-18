#pragma once

#include "Message.h"

using TiltedPhoques::String;

struct PlayerDialogueRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kPlayerDialogueRequest;

    PlayerDialogueRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const PlayerDialogueRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && ActorId == acRhs.ActorId && Token == acRhs.Token && Text == acRhs.Text; }

    enum ActionCode : uint8_t
    {
        Join = 0,
        Leave,
        Suggest,
        Choose
    };

    uint8_t Action{Join};
    uint32_t ActorId{};
    uint32_t Token{};
    TiltedPhoques::String Text{};
};
