#pragma once
#include <string>

struct TransportService;
struct NotifyCharacterSave;

namespace CharacterCheckpoints
{
// Main-game-thread only. File transport is separate from native save/load
// invocation; never call an unverified save-manager address.
void Begin(bool aRequired) noexcept;
void Reset() noexcept;
void Select(TransportService& aTransport, const std::string& acKey) noexcept;
void Receive(const NotifyCharacterSave& acReply) noexcept;
void Tick(TransportService& aTransport) noexcept;
bool CanEnter() noexcept;
void PublishState() noexcept;
}
