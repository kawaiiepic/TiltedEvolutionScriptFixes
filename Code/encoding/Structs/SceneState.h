#pragma once

#include <Structs/GameId.h>
#include <vector>

struct SceneState
{
    GameId Id{};
    GameId Quest{};
    uint32_t Phase{UINT32_MAX}; // Native scene has not entered its first phase yet.
    bool Active{};
    std::vector<uint32_t> CompletedActions;

    bool operator==(const SceneState&) const = default;
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;
};
