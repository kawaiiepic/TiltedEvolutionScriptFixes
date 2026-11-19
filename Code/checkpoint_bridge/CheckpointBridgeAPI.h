#pragma once
#include <cstdint>

// Private versioned bridge between the launcher and its bundled SKSE listener.
// No Skyrim object layouts are exposed by this ABI.
namespace STCheckpointBridge
{
constexpr uint32_t Version = 3;
inline constexpr char LobbySaveName[] = "STLobbyTest";
enum Operation : uint32_t { Load = 1, Save = 2 };
enum Result : uint32_t { Pending = 0, Succeeded = 1, Failed = 2 };

struct Api
{
    uint32_t Size;
    uint32_t Version;
    bool (*Arm)(uint64_t, uint32_t, const char*) noexcept;
    void (*Returned)(uint64_t, bool) noexcept;
    uint32_t (*Poll)(uint64_t) noexcept;
    void (*Abandon)(uint64_t) noexcept;
    uint64_t (*LoadGeneration)() noexcept;
    uint64_t (*NewGameGeneration)() noexcept;
};

using GetApi = const Api* (*)(uint32_t) noexcept;
}
