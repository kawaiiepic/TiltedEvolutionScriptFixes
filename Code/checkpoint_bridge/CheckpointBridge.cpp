#include "CheckpointBridgeAPI.h"

#include <windows.h>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>

namespace
{
// Public SKSE PluginAPI.h ABI, limited to the fields used by this listener.
// This DLL does not resolve or call any game functions.
struct SKSEInterface
{
    uint32_t skseVersion, runtimeVersion, editorVersion, isEditor;
    void* (*QueryInterface)(uint32_t);
    uint32_t (*GetPluginHandle)();
};
struct Messaging
{
    struct Message
    {
        const char* sender;
        uint32_t type, dataLen;
        void* data;
    };
    uint32_t interfaceVersion;
    bool (*RegisterListener)(uint32_t, const char*, void (*)(Message*));
};
struct PluginVersion
{
    uint32_t dataVersion, pluginVersion;
    char name[256], author[256], supportEmail[252];
    uint32_t independenceEx, independence, compatibleVersions[16], seVersionRequired;
};
static_assert(sizeof(PluginVersion) == 0x350);
constexpr uint32_t kMessaging = 5;
constexpr uint32_t kPreLoad = 2, kPostLoad = 3, kSave = 4, kNewGame = 7;
constexpr uint32_t k1170 = 0x01064920, k17104 = 0x01070680;
using namespace STCheckpointBridge;

std::mutex s_mutex;
bool s_registered{};
uint64_t s_loadGeneration{}, s_newGameGeneration{};
struct PendingOperation
{
    uint64_t Token{};
    uint32_t Kind{};
    std::array<char, 64> Name{};
    bool SawNamedEvent{}, SawPost{}, PostSuccess{}, CallReturned{}, CallSuccess{}, Abandoned{}, Mismatch{};
};
PendingOperation s_op;

bool ValidName(std::string_view acName)
{
    if (acName.size() != 35 || !acName.starts_with("STCampaign_"))
        return false;
    for (const char ch : acName.substr(11))
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}
bool Match(const Messaging::Message& acMessage)
{
    if (!acMessage.data || !acMessage.dataLen || acMessage.dataLen > 32768)
        return false;
    std::string_view name(static_cast<const char*>(acMessage.data), acMessage.dataLen);
    if (!name.empty() && name.back() == '\0') name.remove_suffix(1);
    if (name.find('\0') != std::string_view::npos) return false;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
    if (name.size() >= 4 && _strnicmp(name.data() + name.size() - 4, ".ess", 4) == 0)
        name.remove_suffix(4);
    const size_t expected = std::strlen(s_op.Name.data());
    return name.size() == expected && _strnicmp(name.data(), s_op.Name.data(), expected) == 0;
}
uint32_t ResultLocked()
{
    if (!s_op.CallReturned) return Pending;
    if (!s_op.CallSuccess) return Failed;
    if (s_op.Kind == Load)
    {
        if (!s_op.SawPost) return Pending;
        return s_op.SawNamedEvent && s_op.PostSuccess && !s_op.Mismatch ? Succeeded : Failed;
    }
    if (s_op.Mismatch) return Failed;
    // SaveGame is a pre-save notification in SKSE, not a file-completion
    // event. Require the call return as well; the launcher still waits
    // for stable, exclusively readable ESS and co-save files.
    return s_op.SawNamedEvent ? Succeeded : Pending;
}
void DrainAbandoned()
{
    if (s_op.Abandoned && ResultLocked() != Pending) s_op = {};
}
void OnMessage(Messaging::Message* apMessage)
{
    if (!apMessage || !apMessage->sender || std::strcmp(apMessage->sender, "SKSE") != 0) return;
    const auto& msg = *apMessage;
    std::lock_guard lock(s_mutex);
    if (msg.type == kPreLoad || msg.type == kNewGame) ++s_loadGeneration;
    if (msg.type == kNewGame) ++s_newGameGeneration;
    if (!s_op.Token) return;
    if (msg.type == kNewGame)
    {
        s_op.Mismatch = true;
    }
    else if (s_op.Kind == Load && msg.type == kPreLoad)
    {
        if (s_op.SawNamedEvent || !Match(msg)) s_op.Mismatch = true;
        else s_op.SawNamedEvent = true;
    }
    else if (s_op.Kind == Load && msg.type == kPostLoad)
    {
        if (s_op.SawPost) s_op.Mismatch = true;
        s_op.SawPost = true;
        // SKSE dispatches (void*)result, dataLen=1, not bool*.
        s_op.PostSuccess = msg.dataLen == 1 && reinterpret_cast<uintptr_t>(msg.data) == 1;
    }
    else if (s_op.Kind == Save && msg.type == kSave)
    {
        if (s_op.SawNamedEvent || !Match(msg)) s_op.Mismatch = true;
        else s_op.SawNamedEvent = true;
    }
    DrainAbandoned();
}
bool Arm(uint64_t aToken, uint32_t aKind, const char* apName) noexcept
{
    if (!aToken || !apName || (aKind != Load && aKind != Save)) return false;
    const std::string_view name(apName);
    if (!ValidName(name) && !(aKind == Load && name == LobbySaveName)) return false;
    std::lock_guard lock(s_mutex);
    if (!s_registered || s_op.Token) return false;
    s_op.Token = aToken;
    s_op.Kind = aKind;
    std::memcpy(s_op.Name.data(), name.data(), name.size());
    return true;
}
void Returned(uint64_t aToken, bool aSuccess) noexcept
{
    std::lock_guard lock(s_mutex);
    if (s_op.Token != aToken) return;
    s_op.CallReturned = true;
    s_op.CallSuccess = aSuccess;
    DrainAbandoned();
}
uint32_t Poll(uint64_t aToken) noexcept
{
    std::lock_guard lock(s_mutex);
    if (!aToken || s_op.Token != aToken || s_op.Abandoned) return Failed;
    const uint32_t result = ResultLocked();
    if (result != Pending) s_op = {};
    return result;
}
void Abandon(uint64_t aToken) noexcept
{
    std::lock_guard lock(s_mutex);
    if (s_op.Token != aToken) return;
    s_op.Abandoned = true;
    DrainAbandoned();
}
uint64_t LoadGeneration() noexcept
{
    std::lock_guard lock(s_mutex);
    return s_loadGeneration;
}
uint64_t NewGameGeneration() noexcept
{
    std::lock_guard lock(s_mutex);
    return s_newGameGeneration;
}
const Api s_api{sizeof(Api), Version, &Arm, &Returned, &Poll, &Abandon, &LoadGeneration, &NewGameGeneration};
}

extern "C"
{
__declspec(dllexport) PluginVersion SKSEPlugin_Version{
    1, 1, "STCheckpointBridge", "Skyrim Together", "",
    1, 0, {k1170, k17104, 0}, 0
};
__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* apSKSE)
{
    if (!apSKSE || apSKSE->isEditor || (apSKSE->runtimeVersion != k1170 && apSKSE->runtimeVersion != k17104) ||
        !apSKSE->QueryInterface || !apSKSE->GetPluginHandle)
        return false;
    const auto* messaging = static_cast<const Messaging*>(apSKSE->QueryInterface(kMessaging));
    if (!messaging || messaging->interfaceVersion < 2 || !messaging->RegisterListener) return false;
    const uint32_t handle = apSKSE->GetPluginHandle();
    if (handle == UINT32_MAX || !messaging->RegisterListener(handle, "SKSE", &OnMessage)) return false;
    std::lock_guard lock(s_mutex);
    s_registered = true;
    return true;
}
__declspec(dllexport) const STCheckpointBridge::Api* STCheckpointGetApi(uint32_t aVersion) noexcept
{
    std::lock_guard lock(s_mutex);
    return s_registered && aVersion == Version ? &s_api : nullptr;
}
}
