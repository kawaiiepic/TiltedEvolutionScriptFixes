#pragma once

#include "Message.h"

struct NotifyPlayerRespawn final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerRespawn;

    NotifyPlayerRespawn()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyPlayerRespawn& acRhs) const noexcept { return GetOpcode() == acRhs.GetOpcode() && GoldLost == acRhs.GoldLost && DownId == acRhs.DownId && Revived == acRhs.Revived; }

    int32_t GoldLost{};
    uint32_t DownId{};
    bool Revived{};
};
