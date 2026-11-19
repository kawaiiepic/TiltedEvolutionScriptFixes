#include <TiltedOnlinePCH.h>
#include "MainMenuBridge.h"
#include <Games/Skyrim/Interface/UI.h>
#include <Games/Skyrim/Interface/IMenu.h>
#include <VersionDb.h>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace MainMenuBridge
{
namespace
{
// Source-only ABI candidates: SKSE64 ScaleformMovie.h,
// CommonLibSSE-NG GFxMovie.h/GFxValue.h/MainMenu.h and this project's
// IMenu.h. Native calls are not verified against the shipped binaries.
// No managed objects are acquired: only boolean values and a borrowed
// static string argument are passed to the movie.
struct alignas(8) PrimitiveValue
{
    void* Interface{};
    uint32_t Type{};
    uint32_t Pad{};
    union { bool Boolean; const char* String; uint64_t Bits; } Data{};
};
static_assert(sizeof(PrimitiveValue) == 0x18);
static_assert(offsetof(PrimitiveValue, Data) == 0x10);
static_assert(offsetof(IMenu, uiMovie) == 0x10);
using Advance = void (*)(IMenu*, float, uint32_t);
using GetVariable = bool (*)(const void*, PrimitiveValue*, const char*);
using SetVariable = bool (*)(void*, const char*, const PrimitiveValue&, uint32_t);
using IsAvailable = bool (*)(const void*, const char*);
using InvokeNoReturn = void (*)(void*, const char*, const PrimitiveValue*, uint32_t);
constexpr size_t kAdvanceMovie = 5, kIsAvailable = 0xA, kSetVariable = 0x10, kGetVariable = 0x11;
constexpr const char* kProtocol = "_root.STMenuProtocol";
constexpr const char* kSettings = "_root.STSettingsRequested";
constexpr const char* kPlay = "_root.STPlayRequested";
constexpr const char* kStart = "_root.MenuHolder.Menu_mc.FadeOutAndCall";

std::atomic<IMenu*> s_menu{};
std::atomic<Advance> s_original{};
std::atomic_bool s_ready{}, s_play{}, s_settings{};
enum class StartState { Idle, Pending, Dispatched, Failed };
std::atomic<StartState> s_start{StartState::Idle};
InvokeNoReturn s_invoke{};
void** s_slot{};
bool s_attempted{};
unsigned int s_protocolProbeFrames{};
bool s_playResetWarned{};
std::mutex s_movieLock;

bool Accessible(const void* p, size_t size, bool executable = false) noexcept
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto prot = info.Protect & 0xff;
    const bool exec = prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    const bool read = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
        prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    const auto offset = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(info.BaseAddress);
    return offset <= info.RegionSize && size <= info.RegionSize - offset && (executable ? exec : read);
}

template <typename T> T MovieFunction(void* pMovie, size_t aIndex) noexcept
{
    if (!Accessible(pMovie, sizeof(void*))) return nullptr;
    auto** table = *static_cast<void***>(pMovie);
    if (!Accessible(table, (aIndex + 1) * sizeof(void*))) return nullptr;
    return Accessible(table[aIndex], 1, true) ? reinterpret_cast<T>(table[aIndex]) : nullptr;
}

bool ReadBool(void* pMovie, const char* name, bool& result) noexcept
{
    const auto get = MovieFunction<GetVariable>(pMovie, kGetVariable);
    if (!get) return false;
    PrimitiveValue value{};
    value.Type = 0x82; // GFxValue::kConvertBoolean: never acquire a managed string/object.
    if (!get(pMovie, &value, name) || value.Type != 2 || value.Interface) return false;
    result = value.Data.Boolean;
    return true;
}

bool WriteBool(void* pMovie, const char* name, bool flag) noexcept
{
    const auto set = MovieFunction<SetVariable>(pMovie, kSetVariable);
    if (!set) return false;
    PrimitiveValue value{};
    value.Type = 2;
    value.Data.Boolean = flag;
    return set(pMovie, name, value, 0); // normal, not sticky.
}

void HookAdvance(IMenu* pMenu, float interval, uint32_t time)
{
    const auto original = s_original.load();
    if (original) original(pMenu, interval, time);
    // Other menu objects are not touched. The native menu's own UI
    // advance owns the movie; do not call Scaleform from network ticks.
    if (pMenu != s_menu.load()) return;
    std::lock_guard guard(s_movieLock);
    if (pMenu != s_menu.load()) return;
    void* movie = pMenu->uiMovie;
    bool patched = false;
    if (!ReadBool(movie, kProtocol, patched) || !patched)
    {
        s_ready.store(false);
        if (s_protocolProbeFrames < 120 && ++s_protocolProbeFrames == 120)
            spdlog::warn("Native Play menu protocol was not readable after 120 advances");
        return;
    }
    if (!s_ready.exchange(true))
        spdlog::info("Native Play menu protocol detected");
    bool play = false;
    if (ReadBool(movie, kPlay, play) && play)
    {
        if (WriteBool(movie, kPlay, false))
        {
            s_playResetWarned = false;
            s_play.store(true);
        }
        else if (!s_playResetWarned)
        {
            s_playResetWarned = true;
            spdlog::warn("Native Play request was read but could not be cleared");
        }
    }
    bool settings = false;
    if (ReadBool(movie, kSettings, settings) && settings && WriteBool(movie, kSettings, false))
        s_settings.store(true);
    if (s_start.load() != StartState::Pending) return;
    const auto available = MovieFunction<IsAvailable>(movie, kIsAvailable);
    if (!available || !available(movie, kStart) || !s_invoke)
    {
        s_start.store(StartState::Failed);
        return;
    }
    auto expected = StartState::Pending;
    if (!s_start.compare_exchange_strong(expected, StartState::Dispatched)) return;
    // Exact fade-out/callback used by the original menu's confirmed
    // New Game branch. This is a vanilla new game, not a coc shortcut.
    PrimitiveValue arg{};
    arg.Type = 4;
    arg.Data.String = "StartNewGame";
    spdlog::info("Starting vanilla new game for selected server character");
    s_invoke(movie, kStart, &arg, 1);
}

bool Install(IMenu* menu) noexcept
{
    if (!Accessible(menu, 0x18)) return false;
    const auto& db = VersionDb::Get();
    if (db.GetLoadedVersionString() != "1.6.1170.0" && db.GetLoadedVersionString() != "1.7.104.0")
    {
        spdlog::warn("Native Play adapter rejected game version {}", db.GetLoadedVersionString());
        return false;
    }
    auto** table = *reinterpret_cast<void***>(menu);
    if (!Accessible(table, (kAdvanceMovie + 1) * sizeof(void*))) return false;
    auto** slot = table + kAdvanceMovie;
    const auto original = *slot;
    auto* invoke = db.FindAddressById(82665); // GFxMovieView::InvokeNoReturn, AE Address Library ID.
    if (!Accessible(original, 1, true)) return false;
    if (!Accessible(invoke, 1, true))
    {
        spdlog::warn("Native Play adapter: New Game invoke address unavailable; connection requests remain supported");
        invoke = nullptr;
    }
    // Do not stack on an unknown third-party detour or vtable.
    unsigned long long id{};
    if (!db.FindIdByAddress(original, id))
    {
        spdlog::warn("Main menu AdvanceMovie is not a recognized game function; leaving it unchanged");
        return false;
    }
    DWORD protect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protect)) return false;
    s_invoke = reinterpret_cast<InvokeNoReturn>(invoke);
    s_original.store(reinterpret_cast<Advance>(original));
    const auto previous = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(slot), reinterpret_cast<void*>(&HookAdvance), original);
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), protect, &ignored);
    if (previous != original) return false;
    s_slot = slot;
    spdlog::warn("Native main menu adapter installed (source-backed, not binary verified)");
    return true;
}
}

void Tick(bool aMainMenu) noexcept
{
    if (!aMainMenu)
    {
        if (s_menu.exchange(nullptr))
        {
            std::lock_guard guard(s_movieLock);
            s_ready.store(false);
            s_play.store(false);
            s_settings.store(false);
            auto pending = StartState::Pending;
            s_start.compare_exchange_strong(pending, StartState::Failed);
        }
        return;
    }
    auto* ui = UI::Get();
    auto* menu = ui ? ui->FindMenuByName("Main Menu") : nullptr;
    if (!menu || !Accessible(menu, 0x18)) return;
    if (!s_attempted)
    {
        s_attempted = true;
        wchar_t disable[4]{};
        if (GetEnvironmentVariableW(L"ST_DISABLE_NATIVE_MENU", disable, 4) == 1 && disable[0] == L'1')
            return;
        if (!Install(menu))
            spdlog::warn("Native Play menu adapter unavailable; use the overlay hotkey and unmodified menu");
    }
    if (!s_slot || *reinterpret_cast<void***>(menu) + kAdvanceMovie != s_slot) return;
    if (s_menu.load() != menu)
    {
        std::lock_guard guard(s_movieLock);
        s_ready.store(false);
        s_play.store(false);
        s_settings.store(false);
        s_protocolProbeFrames = 0;
        s_playResetWarned = false;
        s_menu.store(menu);
    }
}

bool IsReady() noexcept { return s_ready.load(); }
bool ConsumePlayRequest() noexcept
{
    return s_play.exchange(false);
}
bool ConsumeSettingsRequest() noexcept { return s_settings.exchange(false); }
bool PrefillCharacterName(const char* apName) noexcept
{
    auto* ui = UI::Get();
    if (!s_invoke || !ui || !ui->GetMenuOpen("RaceSex Menu")) return false;
    auto* menu = ui->FindMenuByName("RaceSex Menu");
    if (!Accessible(menu, sizeof(IMenu))) return false;
    void* movie = menu->uiMovie;
    constexpr auto method = "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance.SetNameText";
    const auto available = MovieFunction<IsAvailable>(movie, kIsAvailable);
    if (!available || !available(movie, method)) return false;

    // The supplied RaceMenu SWF copies this display name into its final text entry.
    PrimitiveValue arg{};
    arg.Type = 4;
    arg.Data.String = apName;
    s_invoke(movie, method, &arg, 1);
    spdlog::info("Prefilled the campaign character name in RaceMenu");
    return true;
}

bool HideDialogueMovie(void*& apHiddenMovie) noexcept
{
    // GFxMovie virtual SetVisible/GetVisible slots 8/9 (public SKSE/
    // CommonLib declarations). Like the main-menu adapter, this is a
    // source-backed ABI and needs verification in the shipped runtime.
    const auto& db = VersionDb::Get();
    if (db.GetLoadedVersionString() != "1.7.104.0" && db.GetLoadedVersionString() != "1.6.1170.0")
        return false;
    auto* ui = UI::Get();
    auto* menu = ui && ui->GetMenuOpen("Dialogue Menu") ? ui->FindMenuByName("Dialogue Menu") : nullptr;
    if (!Accessible(menu, sizeof(IMenu))) return false;
    void* movie = menu->uiMovie;
    using SetVisible = void (*)(void*, bool);
    using GetVisible = bool (*)(const void*);
    const auto set = MovieFunction<SetVisible>(movie, 8);
    const auto get = MovieFunction<GetVisible>(movie, 9);
    if (!set || !get) return false;
    if (apHiddenMovie == movie) return !get(movie);
    if (!get(movie)) return false; // another UI mod owns visibility
    set(movie, false);
    if (get(movie)) return false;
    apHiddenMovie = movie;
    return true;
}

void RestoreDialogueMovie(void*& apHiddenMovie) noexcept
{
    void* hidden = apHiddenMovie;
    apHiddenMovie = nullptr;
    if (!hidden) return;
    auto* ui = UI::Get();
    auto* menu = ui && ui->GetMenuOpen("Dialogue Menu") ? ui->FindMenuByName("Dialogue Menu") : nullptr;
    // The native menu may have been destroyed. Do not use the old pointer.
    if (!Accessible(menu, sizeof(IMenu)) || menu->uiMovie != hidden) return;
    using SetVisible = void (*)(void*, bool);
    if (const auto set = MovieFunction<SetVisible>(menu->uiMovie, 8))
        set(menu->uiMovie, true);
}

bool RequestNewGame() noexcept
{
    if (!s_ready.load() || !s_menu.load() || !s_invoke) return false;
    auto idle = StartState::Idle;
    return s_start.compare_exchange_strong(idle, StartState::Pending);
}
bool NewGameRequestFailed() noexcept { return s_start.load() == StartState::Failed; }
void CancelNewGame() noexcept { s_start.store(StartState::Idle); }
}
