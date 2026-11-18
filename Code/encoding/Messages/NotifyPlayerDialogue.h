#pragma once

#include "Message.h"

struct NotifyPlayerDialogue final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerDialogue;

    NotifyPlayerDialogue()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerDialogue& achRhs) const noexcept { return GetOpcode() == achRhs.GetOpcode() && Kind == achRhs.Kind && ActorId == achRhs.ActorId && Token == achRhs.Token && CanChoose == achRhs.CanChoose && Name == achRhs.Name && Text == achRhs.Text; }

    enum KindCode : uint8_t
    {
        State = 0,
        Suggestion,
        Choice,
        Denied
    };

    uint8_t Kind{State};
    uint32_t ActorId{};
    uint32_t Token{};
    bool CanChoose{};
    TiltedPhoques::String Name{};
    TiltedPhoques::String Text{};
};
