#pragma once

#include "Message.h"
#include <Structs/GameId.h>

struct NotifyTravelInvitation final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyTravelInvitation;

    NotifyTravelInvitation() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint32_t Id{};
    uint32_t RemainingMs{};
    uint32_t ParticipantCount{};
    bool CanCancel{};
    bool Accepted{};
    GameId CellId{};
    GameId WorldSpaceId{};
    TiltedPhoques::String LeaderName;
    TiltedPhoques::String NativeRoute;
};
