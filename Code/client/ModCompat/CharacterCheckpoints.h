#pragma once
#include <string>
#include <cstdint>

struct TransportService;
struct NotifyCharacterSave;

namespace CharacterCheckpoints
{
// Main-game-thread only. File transport is separate from native save/load
// invocation; native calls are available only through the experimental
// source-backed driver or a future binary-verified implementation.
// Internal driver contract, NOT a Skyrim ABI. The native adapter registers
// once before activating a checkpoint lobby session.
// Every callback runs on the existing game thread. The adapter owns its
// native request lifetime and must copy the filename before returning.
// Success means the matching named load finished (including game/VM
// initialization), or the matching ESS and required co-save finished.
// Busy means no request was queued. Abandon invalidates the token; if
// the engine cannot cancel an operation, the driver drains it before
// accepting another one. Never report success for an unrelated load.
enum class NativeStart { Busy, Accepted, Failed };
enum class NativeResult { Pending, Succeeded, Failed };
struct NativeDriver
{
    NativeStart (*RequestLoad)(uint64_t aToken, const char* apSaveName) noexcept{};
    NativeStart (*RequestSave)(uint64_t aToken, const char* apSaveName) noexcept{};
    NativeResult (*Poll)(uint64_t aToken) noexcept{};
    void (*Abandon)(uint64_t aToken) noexcept{};
};
bool RegisterNativeDriver(const NativeDriver& acDriver) noexcept;
bool FinishCharacter(TransportService& aTransport) noexcept;
bool IsAlternateStartPickerEnabled() noexcept;
bool CanReplaceStagingWorld() noexcept;
bool NeedsStagingPump() noexcept;

void Begin(bool aRequired) noexcept;
void Reset() noexcept;
void RequestRoster(TransportService& aTransport, const std::string& acOwner, const std::string& acLegacy) noexcept;
void Select(TransportService& aTransport, const std::string& acKey, const std::string& acLabel, bool aCreate) noexcept;
void Receive(const NotifyCharacterSave& acReply) noexcept;
void Tick(TransportService& aTransport) noexcept;
enum class DisconnectAction { Request, Retry, Cancel, Discard };
void Disconnect(TransportService& aTransport, DisconnectAction aAction) noexcept;
void ReturnToMainMenu() noexcept;
bool CanEnter() noexcept;
void PublishState() noexcept;
}
