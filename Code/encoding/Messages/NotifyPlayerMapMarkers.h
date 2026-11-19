#pragma once

#include "Message.h"
#include <Structs/GameId.h>

struct NotifyPlayerMapMarkers final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyPlayerMapMarkers;
    static constexpr size_t MaxPlayers = 32;

    NotifyPlayerMapMarkers() : ServerMessage(Opcode) {}

    struct PlayerMarker
    {
        uint32_t Id{};
        TiltedPhoques::String Name;
        GameId Cell{};
        GameId WorldSpace{};
        GameId Entrance{};
        GameId EntranceWorldSpace{};
        glm::vec3 Position{};
        glm::vec3 EntrancePosition{};
    };

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;

    uint64_t Tick{};
    TiltedPhoques::Vector<PlayerMarker> Players;
};
