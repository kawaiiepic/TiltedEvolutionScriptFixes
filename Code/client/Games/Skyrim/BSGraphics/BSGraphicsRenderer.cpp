
#include "Services/InputService.h"
#include "Systems/RenderSystemD3D11.h"

#include "World.h"
#include <atomic>

#include "BSGraphics/BSGraphicsRenderer.h"
#include "BSRandom/BSRandom.h"

// shared resource by launcher
extern HICON g_SharedWindowIcon;

namespace BSGraphics
{
namespace
{

static RenderSystemD3D11* g_sRs = nullptr;
static WNDPROC RealWndProc = nullptr;
static RendererWindow* g_RenderWindow = nullptr;
static std::atomic<HWND> s_menuPumpWindow{nullptr};
static UINT_PTR s_menuPumpTimer = 0;

// Use a process-independent registered window message rather than taking
// an arbitrary WM_APP value that another installed mod might already use.
UINT MenuPumpMessage() noexcept
{
    static const UINT s_message = RegisterWindowMessageW(L"SkyrimTogether.Campaign.MenuPump.v1");
    return s_message;
}

static constexpr char kTogetherWindowName[]{"Skyrim Together"};

void CALLBACK MenuPumpTimer(HWND aWindow, UINT, UINT_PTR aId, DWORD)
{
    if (aWindow == s_menuPumpWindow.load(std::memory_order_acquire) && aId == s_menuPumpTimer)
    {
        World::PumpMainMenu();
        InputService::RefreshSystemCursor(aWindow);
    }
}

} // namespace

void WakeMainMenuPump() noexcept
{
    const HWND window = s_menuPumpWindow.load(std::memory_order_acquire);
    const UINT message = MenuPumpMessage();
    if (!window || !message) return;
    if (!PostMessageW(window, message, 0, 0))
    {
        const DWORD error = GetLastError();
        static std::atomic_bool s_loggedPostFailure{false};
        if (!s_loggedPostFailure.exchange(true))
            spdlog::warn("Could not post the Skyrim Together menu wake-up (Windows error {})", error);
    }
}

RendererWindow* GetMainWindow()
{
    return g_RenderWindow;
}

bool RendererWindow::IsForeground()
{
    return GetForegroundWindow() == hWnd;
}

void (*Renderer_Init)(Renderer*, BSGraphics::RendererInitOSData*, const BSGraphics::ApplicationWindowProperties*, BSGraphics::RendererInitReturn*) = nullptr;

// WNDPROC seems to be part of the renderer
LRESULT CALLBACK Hook_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (hwnd == s_menuPumpWindow.load(std::memory_order_acquire))
    {
        const UINT message = MenuPumpMessage();
        if (message && uMsg == message)
        {
            World::PumpMainMenu();
            return 0;
        }
        if (uMsg == WM_TIMER && s_menuPumpTimer && wParam == s_menuPumpTimer)
        {
            World::PumpMainMenu();
            return 0;
        }
        if (uMsg == WM_NCDESTROY)
        {
            s_menuPumpWindow.store(nullptr, std::memory_order_release);
            if (s_menuPumpTimer) KillTimer(hwnd, s_menuPumpTimer);
            s_menuPumpTimer = 0;
        }
    }
    if (const auto handled = InputService::WndProc(hwnd, uMsg, wParam, lParam); handled != 0)
        return handled;

    return RealWndProc(hwnd, uMsg, wParam, lParam);
}

void Hook_Renderer_Init(Renderer* self, BSGraphics::RendererInitOSData* aOSData, const BSGraphics::ApplicationWindowProperties* aFBData, BSGraphics::RendererInitReturn* aOut)
{
    // we feed this a shared icon as the resource directory of our former launcher data is already overwritten with the
    // game.
    aOSData->hIcon = g_SharedWindowIcon;
    // Append our window name.
    aOSData->pClassName = kTogetherWindowName;

    RealWndProc = aOSData->pWndProc;
    aOSData->pWndProc = Hook_WndProc;

    Renderer_Init(self, aOSData, aFBData, aOut);

    g_sRs = &World::Get().ctx().at<RenderSystemD3D11>();
    // This how the game does it too
    g_RenderWindow = &self->Data.RenderWindowA[0];

    const BSGraphics::RendererData& renderer = self->Data;

    g_sRs->OnDeviceCreation(renderer.RenderWindowA[0].pSwapChain, renderer.pForwarder, renderer.pContext);

    // Title-screen work runs on the window owner, not inside physics,
    // rendering or the engine's MainLoop detour chain.
    const HWND oldWindow = s_menuPumpWindow.exchange(nullptr, std::memory_order_acq_rel);
    if (s_menuPumpTimer && oldWindow)
        KillTimer(oldWindow, s_menuPumpTimer);
    HWND window = g_sRs->GetWindow();
    if (!window) window = g_RenderWindow->hWnd;
    const DWORD windowThread = window ? GetWindowThreadProcessId(window, nullptr) : 0;
    World::SetMenuWindowThread(windowThread);
    spdlog::info("Skyrim Together window owner thread: {} (renderer init thread {})", windowThread, GetCurrentThreadId());
    s_menuPumpWindow.store(window, std::memory_order_release);
    s_menuPumpTimer = window
        ? SetTimer(window, reinterpret_cast<UINT_PTR>(&s_menuPumpTimer), 50, &MenuPumpTimer)
        : 0;
    if (s_menuPumpTimer)
        spdlog::info("Skyrim Together main-menu window timer installed");
    else
        spdlog::warn("Skyrim Together main-menu window timer unavailable");
    // uiReady may have arrived before renderer initialization completed.
    WakeMainMenuPump();
}

void (*StopTimer)(int) = nullptr;

// Insert us at the End
void Hook_StopTimer(int type)
{
    if (g_sRs)
        g_sRs->OnRender();

    StopTimer(type);
}

static TiltedPhoques::Initializer s_viewportHooks(
    []()
    {
        const VersionDbPtr<void> initWindowLoc(77226);
        // patch dwStyle in BSGraphics::InitWindows
        TiltedPhoques::Put(mem::pointer(initWindowLoc.GetPtr()) + 0x174 + 1, WS_OVERLAPPEDWINDOW);

        const VersionDbPtr<void> windowLoc(68781);
        // TODO: move me to input patches.
        // don't let the game steal the media keys in windowed mode
        TiltedPhoques::Put(
            mem::pointer(windowLoc.GetPtr()) + 0x55 + 2,
            /*strip DISCL_EXCLUSIVE bits and append DISCL_NONEXCLUSIVE*/ 3);

        const VersionDbPtr<void> timerLoc(77246);
        const VersionDbPtr<void> renderInit(77226);

        TiltedPhoques::SwapCall(mem::pointer(timerLoc.GetPtr()) + 9, StopTimer, &Hook_StopTimer);

        Renderer_Init = static_cast<decltype(Renderer_Init)>(renderInit.GetPtr());

        // Once we find a proper way to locate it for different versions, go back to swapcall
        // TiltedPhoques::SwapCall(mem::pointer(initLoc.GetPtr()) + 0xD1A, Renderer_Init, &Hook_Renderer_Init);
        TP_HOOK_IMMEDIATE(&Renderer_Init, &Hook_Renderer_Init);
    });
} // namespace BSGraphics
