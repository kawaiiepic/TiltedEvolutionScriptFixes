#pragma once

#include "Message.h"
#include <vector>

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

    bool operator==(const PlayerDialogueRequest& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && Action == acRhs.Action && ActorId == acRhs.ActorId && Token == acRhs.Token && Revision == acRhs.Revision && Index == acRhs.Index && Options == acRhs.Options && Text == acRhs.Text; }

    enum ActionCode : uint8_t
    {
        Join = 0,
        Leave,
        Suggest,
        Choose,
        PublishOptions,
        Vote,
        SetPrivacy
    };

    uint8_t Action{Join};
    uint32_t ActorId{};
    uint32_t Token{};
    uint32_t Revision{};
    uint32_t Index{};
    std::vector<TiltedPhoques::String> Options;
    TiltedPhoques::String Text{};
};
