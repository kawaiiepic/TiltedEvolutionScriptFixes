#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

struct RequestFastTravelCatchUp final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestFastTravelCatchUp;

    RequestFastTravelCatchUp() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint32_t Sequence{};
    GameId Origin{};
    GameId Destination{};
    Vector3_NetQuantize Position{};
};
