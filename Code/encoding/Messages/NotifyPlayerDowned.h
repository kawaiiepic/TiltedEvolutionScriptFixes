#pragma once

#include "Message.h"

struct NotifyPlayerDowned final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerDowned;

    NotifyPlayerDowned() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint32_t ActorId{};
    uint32_t DownId{}; // Zero clears the downed state.
};
