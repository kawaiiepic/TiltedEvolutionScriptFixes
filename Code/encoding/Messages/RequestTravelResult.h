#pragma once

#include "Message.h"

struct RequestTravelResult final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestTravelResult;
    enum Result : uint8_t { Arrived, Busy, Failed, Interrupted };
    RequestTravelResult() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    uint32_t Id{};
    uint8_t Outcome{Failed};
};
