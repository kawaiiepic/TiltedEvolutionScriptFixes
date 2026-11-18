#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <vector>

struct SharedQuestUpdate
{
    enum StatusCode : uint8_t { StageUpdate, Started, Stopped, Completed };

    GameId Id{};
    uint16_t Stage{};
    uint8_t Status{};

    bool operator==(const SharedQuestUpdate&) const = default;
};

struct NotifyQuestUpdate final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyQuestUpdate;
    static constexpr size_t kMaxUpdates = 16;

    NotifyQuestUpdate() : ServerMessage(Opcode) {}

    void SerializeRaw(TiltedPhoques::Buffer::Writer&) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader&) noexcept override;

    uint64_t Through{};
    std::vector<SharedQuestUpdate> Updates;
};
