#include <TiltedOnlinePCH.h>
#include "NativeCheckpointDriver.h"
#include "CharacterCheckpoints.h"
#include "../../checkpoint_bridge/CheckpointBridgeAPI.h"

#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Services/TransportService.h>
#include <Services/OverlayService.h>
#include <VersionDb.h>
#include <World.h>
#include <OverlayApp.hpp>
#include <OverlayRenderHandler.hpp>
#include <DInputHook.hpp>
#include <array>
#include <cstring>

namespace NativeCheckpointDriver
{
namespace
{
using CharacterCheckpoints::NativeStart;
using CharacterCheckpoints::NativeResult;
namespace Bridge = STCheckpointBridge;
using LoadFunction = bool (*)(void*, const char*, int32_t, uint32_t, bool);
using SaveFunction = bool (*)(void*, int32_t, uint32_t, const char*);
const Bridge::Api* s_bridge{};
void** s_manager{};
LoadFunction s_load{};
SaveFunction s_save{};
uint64_t s_token{}, s_generation{};
bool s_called{}, s_registered{}, s_observeWorld{};
std::array<char, 64> s_name{};

bool IsCommitted(const void* apAddress, bool aExecutable, size_t aBytes = 1)
{
    if (!apAddress) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(apAddress, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = info.Protect & 0xff;
    const bool executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const auto address = reinterpret_cast<uintptr_t>(apAddress);
    const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return address >= base && aBytes <= info.RegionSize && address - base <= info.RegionSize - aBytes &&
        (aExecutable ? executable : readable);
}

bool BusyForSave(TransportService& aTransport)
{
    if (!aTransport.CanEnterWorld()) return true;
    auto* ui = UI::Get();
    auto* player = PlayerCharacter::Get();
    if (!ui || !player || player->IsDead() || player->IsInCombat()) return true;
    static const char* const menus[] = {
        "Main Menu", "Loading Menu", "RaceSex Menu", "Dialogue Menu",
        "MessageBoxMenu", "Journal Menu", "Console", "InventoryMenu",
        "ContainerMenu", "BarterMenu", "Crafting Menu", "MagicMenu",
        "MapMenu", "StatsMenu", "Sleep/Wait Menu", "TweenMenu", "FavoritesMenu"
    };
    for (const char* menu : menus)
        if (ui->GetMenuOpen(menu)) return true;
    return false;
}
void CloseOverlay()
{
    auto& overlay = World::Get().GetOverlayService();
    if (!overlay.GetActive()) return;
    overlay.SetActive(false);
    TiltedPhoques::DInputHook::Get().SetEnabled(false);
    if (auto* app = overlay.GetOverlayApp())
        if (const auto client = app->GetClient())
            if (const auto renderer = client->GetOverlayRenderHandler())
                renderer->SetCursorVisible(false);
}
NativeStart Start(uint64_t aToken, const char* apName, uint32_t aKind) noexcept
{
    if (!s_bridge || !aToken || s_token || !apName) return NativeStart::Busy;
    auto& transport = World::Get().GetTransport();
    auto* ui = UI::Get();
    if (!ui || ui->GetMenuOpen("Loading Menu") || ui->GetMenuOpen("MessageBoxMenu"))
        return NativeStart::Busy;
    if (aKind == Bridge::Load)
    {
        // Replace only a title-screen or client-owned staging session.
        if (!transport.IsInLobby() || (!ui->GetMenuOpen("Main Menu") && !CharacterCheckpoints::CanReplaceStagingWorld()))
            return NativeStart::Failed;
    }
    else if (BusyForSave(transport)) return NativeStart::Busy;
    if (std::strlen(apName) >= s_name.size()) return NativeStart::Failed;
    if (!s_bridge->Arm(aToken, aKind, apName)) return NativeStart::Busy;
    s_name.fill(0);
    std::memcpy(s_name.data(), apName, std::strlen(apName));
    s_token = aToken;
    s_called = false;
    // Avoid calling an engine save/load while TiltedConnect is polling
    // network packets or from the VM worker. Dispatch via the game window.
    World::Get().QueueCheckpointTask([aToken, aKind]()
    {
        if (s_token != aToken) return;
        auto& transport = World::Get().GetTransport();
        auto* ui = UI::Get();
        const bool valid = aKind == Bridge::Load ?
            transport.IsInLobby() && ui &&
                (ui->GetMenuOpen("Main Menu") || CharacterCheckpoints::CanReplaceStagingWorld()) &&
                !ui->GetMenuOpen("Loading Menu") && !ui->GetMenuOpen("MessageBoxMenu") :
            (transport.IsInLobby() || transport.IsOnline()) && !BusyForSave(transport);
        s_called = true;
        if (!valid || !IsCommitted(s_manager, false, sizeof(void*)) ||
            !IsCommitted(*s_manager, false, sizeof(void*)))
        {
            s_bridge->Returned(aToken, false);
            return;
        }
        CloseOverlay();
        spdlog::info("Experimental native checkpoint {} requested", aKind == Bridge::Load ? "load" : "save");
        // Source-backed CommonLibSSE-NG wrappers; not a binary-verified
        // ABI. Retain native mod mismatch checks and default device IDs.
        // Do not interpret the bool return as a complete save/load.
        const bool accepted = aKind == Bridge::Load ?
            s_load(*s_manager, s_name.data(), -1, 0, true) :
            s_save(*s_manager, 2, 0, s_name.data());
        spdlog::info("Native checkpoint {} returned (accepted: {})", aKind == Bridge::Load ? "load" : "save", accepted);
        s_bridge->Returned(aToken, accepted);
    });
    return NativeStart::Accepted;
}
NativeStart RequestLoad(uint64_t aToken, const char* apName) noexcept { return Start(aToken, apName, Bridge::Load); }
NativeStart RequestSave(uint64_t aToken, const char* apName) noexcept { return Start(aToken, apName, Bridge::Save); }
NativeResult Poll(uint64_t aToken) noexcept
{
    if (s_token != aToken || !s_bridge) return NativeResult::Failed;
    const uint32_t result = s_bridge->Poll(aToken);
    if (result == Bridge::Pending) return NativeResult::Pending;
    s_token = 0;
    s_called = false;
    s_generation = s_bridge->LoadGeneration();
    return result == Bridge::Succeeded ? NativeResult::Succeeded : NativeResult::Failed;
}
void Abandon(uint64_t aToken) noexcept
{
    if (s_token != aToken || !s_bridge) return;
    if (!s_called) s_bridge->Returned(aToken, false);
    s_bridge->Abandon(aToken);
    s_token = 0;
    s_called = false;
}
}

bool TryRegister() noexcept
{
    if (s_registered) return true;
    // Native checkpoint automation is automatic for checkpoint servers.
    // Keep an escape hatch for diagnosing runtime/mod compatibility.
    wchar_t disable[4]{};
    if (GetEnvironmentVariableW(L"ST_DISABLE_NATIVE_SAVES", disable, 4) == 1 && disable[0] == L'1')
        return false;
    const auto& db = VersionDb::Get();
    if (db.GetLoadedVersionString() != "1.6.1170.0" && db.GetLoadedVersionString() != "1.7.104.0")
        return false;
    const HMODULE bridge = GetModuleHandleW(L"STCheckpointBridge.dll");
    if (!bridge)
    {
        spdlog::warn("Native checkpoint bridge unavailable; using manual checkpoint loading and saving");
        return false;
    }
    const auto get = reinterpret_cast<Bridge::GetApi>(GetProcAddress(bridge, "STCheckpointGetApi"));
    if (!get) return false;
    const auto* api = get(Bridge::Version);
    if (!api || api->Size < sizeof(Bridge::Api) || api->Version != Bridge::Version ||
        !api->Arm || !api->Returned || !api->Poll || !api->Abandon || !api->LoadGeneration || !api->NewGameGeneration) return false;
    // IDs from CommonLibSSE-NG, not fixed process addresses.
    void* manager = db.FindAddressById(403340);
    void* load = db.FindAddressById(35728);
    void* save = db.FindAddressById(35727);
    if (!IsCommitted(manager, false, sizeof(void*)) || !IsCommitted(load, true) || !IsCommitted(save, true))
    {
        spdlog::warn("Native checkpoint address library candidates unavailable");
        return false;
    }
    s_manager = static_cast<void**>(manager);
    s_load = reinterpret_cast<LoadFunction>(load);
    s_save = reinterpret_cast<SaveFunction>(save);
    s_bridge = api;
    const CharacterCheckpoints::NativeDriver driver{&RequestLoad, &RequestSave, &Poll, &Abandon};
    s_registered = CharacterCheckpoints::RegisterNativeDriver(driver);
    if (s_registered)
        spdlog::warn("Experimental native checkpoint adapter enabled; engine ABI is not binary-verified");
    return s_registered;
}

uint64_t NewGameGeneration() noexcept { return s_bridge ? s_bridge->NewGameGeneration() : 0; }
uint64_t LoadGeneration() noexcept { return s_bridge ? s_bridge->LoadGeneration() : 0; }

void CheckForUnexpectedLoad() noexcept
{
    if (!s_registered || !s_bridge) return;
    auto& transport = World::Get().GetTransport();
    const uint64_t generation = s_bridge->LoadGeneration();
    if (!transport.IsOnline())
    {
        s_observeWorld = false;
        s_generation = generation;
        return;
    }
    if (!s_observeWorld)
    {
        s_observeWorld = true;
        s_generation = generation;
        return;
    }
    if (generation != s_generation)
    {
        s_observeWorld = false;
        spdlog::warn("Unexpected native load/new game while connected; disconnecting");
        transport.Close();
    }
}
}
