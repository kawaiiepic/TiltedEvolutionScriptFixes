#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Vector3_NetQuantize.h>

struct RequestCarriageTravel final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestCarriageTravel;

    RequestCarriageTravel() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    GameId Seat{};
    GameId Cell{};
    Vector3_NetQuantize Position{};
    uint32_t Token{};
    bool Arrived{};
};
