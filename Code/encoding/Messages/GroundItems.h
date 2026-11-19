#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

struct RequestGroundItems final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestGroundItems;
    RequestGroundItems() : ClientMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;

    GameId Cell{};
    uint64_t Id{}; // Zero requests the current cell's drops.
};

struct NotifyGroundItem final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyGroundItem;
    NotifyGroundItem() : ServerMessage(Opcode) {}

    enum Action : uint8_t { Spawn, Remove, Award, RejectDrop, RejectClaim, SnapshotComplete };

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;

    uint8_t Kind{};
    uint64_t Id{};
    uint32_t LocalReference{}; // Echoed only to the originating client.
    GameId Cell{};
    Inventory::Entry Item{};
    glm::vec3 Position{0.f};
    glm::vec3 Rotation{0.f};
};
