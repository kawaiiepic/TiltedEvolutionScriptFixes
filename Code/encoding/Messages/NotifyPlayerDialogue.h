#pragma once

#include "Message.h"
#include <vector>

struct NotifyPlayerDialogue final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerDialogue;

    NotifyPlayerDialogue()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerDialogue& achRhs) const noexcept { return GetOpcode() == achRhs.GetOpcode() && Kind == achRhs.Kind && ActorId == achRhs.ActorId && Token == achRhs.Token && CanChoose == achRhs.CanChoose && Revision == achRhs.Revision && Options == achRhs.Options && Votes == achRhs.Votes && Selected == achRhs.Selected && Name == achRhs.Name && Text == achRhs.Text; }

    enum KindCode : uint8_t
    {
        State = 0,
        Suggestion,
        Choice,
        Denied,
        Panel,
        PanelClosed
    };

    uint8_t Kind{State};
    uint32_t ActorId{};
    uint32_t Token{};
    bool CanChoose{};
    uint32_t Revision{};
    uint32_t Selected{UINT32_MAX};
    std::vector<TiltedPhoques::String> Options;
    std::vector<uint32_t> Votes;
    TiltedPhoques::String Name{};
    TiltedPhoques::String Text{};
};
