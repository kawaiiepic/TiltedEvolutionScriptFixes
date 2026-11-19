#pragma once

namespace MainMenuBridge
{
// Main game thread: observe the active native main menu. The Scaleform
// calls run exclusively in that menu's AdvanceMovie callback.
void Tick(bool aMainMenu) noexcept;
bool IsReady() noexcept;
bool ConsumePlayRequest() noexcept;
bool ConsumeSettingsRequest() noexcept;
bool PrefillCharacterName(const char* apName) noexcept;
// Only hide the presentation, never close or pause the actual conversation.
bool HideDialogueMovie(void*& apHiddenMovie) noexcept;
void RestoreDialogueMovie(void*& apHiddenMovie) noexcept;
bool RequestNewGame() noexcept;
bool NewGameRequestFailed() noexcept;
void CancelNewGame() noexcept;
}
