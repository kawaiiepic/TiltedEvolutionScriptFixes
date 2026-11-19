#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <vector>

struct RequestMapDiscovery final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestMapDiscovery;
    RequestMapDiscovery() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    GameId Marker{};
};

struct NotifyMapDiscoveries final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyMapDiscoveries;
    static constexpr size_t MaxMarkers = 64;
    NotifyMapDiscoveries() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    std::vector<GameId> Markers;
};
