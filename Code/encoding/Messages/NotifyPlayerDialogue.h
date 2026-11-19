#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <vector>
#include <utility>

struct NotifyPlayerDialogue final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerDialogue;

    NotifyPlayerDialogue()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerDialogue& achRhs) const noexcept { return GetOpcode() == achRhs.GetOpcode() && Kind == achRhs.Kind && ActorId == achRhs.ActorId && SpeakerReference == achRhs.SpeakerReference && Token == achRhs.Token && CanChoose == achRhs.CanChoose && CanVote == achRhs.CanVote && CanPass == achRhs.CanPass && Private == achRhs.Private && Remote == achRhs.Remote && Listening == achRhs.Listening && Revision == achRhs.Revision && Options == achRhs.Options && Votes == achRhs.Votes && VoterChoices == achRhs.VoterChoices && Selected == achRhs.Selected && Name == achRhs.Name && Text == achRhs.Text; }

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
    GameId SpeakerReference{};
    uint32_t Token{};
    bool CanChoose{};
    bool CanVote{};
    bool CanPass{};
    bool Private{};
    bool Remote{};
    bool Listening{};
    uint32_t Revision{};
    uint32_t Selected{UINT32_MAX};
    std::vector<TiltedPhoques::String> Options;
    std::vector<uint32_t> Votes;
    // Public conversations: (server player id, owner option index).
    static constexpr uint32_t MaxVoterChoices = 255;
    std::vector<std::pair<uint32_t, uint32_t>> VoterChoices;
    TiltedPhoques::String Name{};
    TiltedPhoques::String Text{};
};
