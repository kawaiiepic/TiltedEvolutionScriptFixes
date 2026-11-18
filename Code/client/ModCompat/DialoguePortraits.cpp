#include <TiltedOnlinePCH.h>
#include "DialoguePortraits.h"
#include "Phantasm_API.h"

#include <World.h>
#include <Actor.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <OverlayApp.hpp>
#include <include/cef_parser.h>
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace DialoguePortraits
{
namespace
{
using namespace PHANTASM_API;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kSize = 96;
constexpr size_t kCacheLimit = 32;
constexpr size_t kActiveLimit = 16;

// One outstanding request at a time. The API returns on its own game-thread
// callback; only copy callback-owned pixels there. Do not retain engine pointers.
struct Entry
{
    uint32_t FormId{};
    Clock::time_point RetryAfter{};
    Clock::time_point LastWanted{};
    std::vector<uint8_t> Pixels;
};
struct Shot
{
    uint32_t Request{};
    uint32_t Player{};
    uint32_t Form{};
};
const Interface1* s_pApi{};
ClientId s_client{};
Shot s_shot{};
std::map<uint32_t, std::set<uint32_t>> s_panels;
std::map<uint32_t, Entry> s_cache;
Clock::time_point s_nextUpdate{};
Clock::time_point s_nextDiscovery{};

void OnEvent(const Event*, void*) {}
void OnStill(const Still* apStill, void*)
{
    if (!apStill || apStill->size < offsetof(Still, rgba) + sizeof(apStill->rgba) ||
        !s_shot.Request || apStill->requestId != s_shot.Request)
        return;

    const Shot shot = s_shot;
    s_shot = {};
    const auto it = s_cache.find(shot.Player);
    if (it == s_cache.end() || it->second.FormId != shot.Form)
        return;

    auto& entry = it->second;
    entry.RetryAfter = Clock::now() + std::chrono::seconds(10);
    if (!apStill->ok || !apStill->rgba || apStill->width != kSize || apStill->height != kSize)
        return;

    // Phantasm supplies sRGB, linear-premultiplied-alpha pixels. The UI
    // converts to straight alpha before constructing its cached PNG.
    entry.Pixels.assign(apStill->rgba, apStill->rgba + kSize * kSize * 4);
    entry.RetryAfter = Clock::now() + std::chrono::seconds(90);
}

bool Discover() noexcept
{
    if (s_pApi && s_client)
        return true;
    const auto now = Clock::now();
    if (now < s_nextDiscovery)
        return false;
    s_nextDiscovery = now + std::chrono::seconds(15);

    const HMODULE module = GetModuleHandleW(L"Phantasm.dll");
    if (!module)
        return false;
    const auto get = reinterpret_cast<RequestPluginAPIFn>(GetProcAddress(module, "RequestPluginAPI"));
    if (!get)
        return false;
    const auto* pApi = static_cast<const Interface1*>(get(kInterface1));
    if (!pApi || pApi->size < offsetof(Interface1, RequestStillHinted) + sizeof(pApi->RequestStillHinted) ||
        pApi->interfaceVersion != kInterface1 || !pApi->RegisterClient || !pApi->RequestStillHinted)
        return false;
    ClientId client{};
    if (pApi->RegisterClient("skyrim-together", &OnEvent, nullptr, &client) != Result::Ok || !client)
        return false;
    s_pApi = pApi;
    s_client = client;
    spdlog::info("Optional Phantasm character portrait API available");
    return true;
}

void CancelShot() noexcept
{
    const uint32_t request = s_shot.Request;
    s_shot = {};
    if (request && s_pApi && s_pApi->CancelStill)
        s_pApi->CancelStill(s_client, request);
}

void Send(TiltedPhoques::OverlayApp* apOverlay, uint32_t aPlayerId, const std::vector<uint8_t>& acPixels) noexcept
{
    if (!apOverlay)
        return;
    auto args = CefListValue::Create();
    args->SetDouble(0, aPlayerId);
    args->SetInt(1, kSize);
    args->SetInt(2, kSize);
    args->SetString(3, acPixels.empty() ? CefString{} : CefBase64Encode(acPixels.data(), acPixels.size()));
    apOverlay->ExecuteAsync("characterPortrait", args);
}
}

void Observe(const NotifyPlayerDialogue& acMessage) noexcept
{
    if (acMessage.Kind != NotifyPlayerDialogue::Panel && acMessage.Kind != NotifyPlayerDialogue::PanelClosed)
        return;
    s_panels.erase(acMessage.ActorId);
    if (acMessage.Kind == NotifyPlayerDialogue::PanelClosed || acMessage.Private || acMessage.VoterChoices.empty())
        return;
    if (s_panels.size() >= kCacheLimit)
        return;
    auto& voters = s_panels[acMessage.ActorId];
    for (const auto& [id, index] : acMessage.VoterChoices)
    {
        if (index < acMessage.Options.size() && voters.size() < kActiveLimit)
            voters.insert(id);
    }
}

void Update(World& aWorld, TiltedPhoques::OverlayApp* apOverlay, uint32_t aLocalPlayerId) noexcept
{
    const auto now = Clock::now();
    if (!apOverlay || now < s_nextUpdate)
        return;
    s_nextUpdate = now + std::chrono::milliseconds(500);
    if (s_panels.empty() || !Discover())
        return;

    std::set<uint32_t> wanted;
    for (const auto& [actorId, voters] : s_panels)
    {
        for (const uint32_t id : voters)
        {
            if (wanted.size() >= kActiveLimit)
                break;
            wanted.insert(id);
        }
    }
    if (s_shot.Request && !wanted.contains(s_shot.Player))
        CancelShot();

    for (const uint32_t id : wanted)
    {
        uint32_t formId = id == aLocalPlayerId ? 0x14 : 0;
        if (!formId)
        {
            const auto view = aWorld.view<PlayerComponent, FormIdComponent>(entt::exclude<WaitingFor3D>);
            for (const auto entity : view)
            {
                if (view.get<PlayerComponent>(entity).Id == id)
                {
                    const uint32_t candidate = view.get<FormIdComponent>(entity).Id;
                    if (Cast<Actor>(TESForm::GetById(candidate)))
                        formId = candidate;
                    break;
                }
            }
        }
        if (!formId)
            continue;

        auto it = s_cache.find(id);
        if (it == s_cache.end())
        {
            if (s_cache.size() >= kCacheLimit)
            {
                auto oldest = s_cache.end();
                for (auto candidate = s_cache.begin(); candidate != s_cache.end(); ++candidate)
                {
                    if (wanted.contains(candidate->first))
                        continue;
                    if (oldest == s_cache.end() || candidate->second.LastWanted < oldest->second.LastWanted)
                        oldest = candidate;
                }
                if (oldest == s_cache.end())
                    continue;
                Send(apOverlay, oldest->first, {});
                s_cache.erase(oldest);
            }
            it = s_cache.try_emplace(id).first;
        }
        auto& entry = it->second;
        entry.LastWanted = now;
        if (entry.FormId != formId)
        {
            if (s_shot.Request && s_shot.Player == id)
                CancelShot();
            entry.FormId = formId;
            entry.Pixels.clear();
            entry.RetryAfter = {};
            Send(apOverlay, id, {});
        }
        if (!entry.Pixels.empty())
        {
            Send(apOverlay, id, entry.Pixels);
            entry.Pixels.clear();
        }
        if (s_shot.Request || now < entry.RetryAfter)
            continue;
        uint32_t request{};
        entry.RetryAfter = now + std::chrono::seconds(10);
        const auto result = s_pApi->RequestStillHinted(s_client, formId, kSize, kSize,
            Framing::Portrait, "lighting=studio;aim=face;zoom=1.6", &OnStill, nullptr, &request);
        if (result == Result::Ok && request)
            s_shot = {request, id, formId};
    }
}

void RemovePlayer(uint32_t aPlayerId) noexcept
{
    if (s_shot.Request && s_shot.Player == aPlayerId)
        CancelShot();
    s_cache.erase(aPlayerId);
    for (auto& [actorId, voters] : s_panels)
        voters.erase(aPlayerId);
}

void Reset() noexcept
{
    CancelShot();
    s_cache.clear();
    s_panels.clear();
    s_nextUpdate = {};
    // Keep the single optional API registration for process lifetime.
    // No callback holds a World, OverlayService or actor pointer.
}
}
