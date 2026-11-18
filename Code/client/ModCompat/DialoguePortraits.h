#pragma once

#include <cstdint>

struct World;
struct NotifyPlayerDialogue;
namespace TiltedPhoques { struct OverlayApp; }

namespace DialoguePortraits
{
// Optional Phantasm public API consumer. All operations run on the game
// thread; no native menu data or character appearance data is modified.
void Observe(const NotifyPlayerDialogue& acMessage) noexcept;
void Update(World& aWorld, TiltedPhoques::OverlayApp* apOverlay, uint32_t aLocalPlayerId) noexcept;
void RemovePlayer(uint32_t aPlayerId) noexcept;
void Reset() noexcept;
}
