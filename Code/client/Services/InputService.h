#pragma once

struct OverlayService;

/**
 * @brief Handles input handling for the UI.
 */
struct InputService
{
    InputService(OverlayService& aOverlay) noexcept;
    ~InputService() noexcept;

    static bool SetOverlayActive(OverlayService& aOverlay, bool aActive) noexcept;
    static void SetWorldCursorSuppressed(bool aSuppressed) noexcept;
    static void RefreshSystemCursor(HWND aWindow) noexcept;
    static void PollMainMenuHotkey(OverlayService& aOverlay, HWND aWindow) noexcept;

    static LRESULT WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    TP_NOCOPYMOVE(InputService);
};
