#include <TiltedOnlinePCH.h>

#include <ModCompat/PlayerVoiceSync.h>
#include <Services/PluginMessagingService.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Games/ActorExtension.h>
#include <Actor.h>
#include <World.h>
#include "../../checkpoint_bridge/CheckpointBridgeAPI.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr char kChannel[] = "skyrimtogether.player.voice.v1";
constexpr size_t kMaxPath = 511;
constexpr uint8_t kStart = 1, kStop = 2;
constexpr size_t kHeaderSize = 5;

std::string VoicePath(std::string_view aPath)
{
    if (aPath.empty() || aPath.size() > kMaxPath ||
        std::ranges::any_of(aPath, [](unsigned char c)
        { return c < 32 || c == 127 || c == ':' || c == '"' || c == '*' ||
            c == '?' || c == '<' || c == '>' || c == '|'; }))
        return {};
    std::string path(aPath);
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.front() == '\\' || path.back() == '\\')
        return {};
    size_t begin = 0;
    while (begin < path.size())
    {
        const auto end = path.find('\\', begin);
        const auto part = std::string_view(path).substr(begin, end == path.npos ? path.npos : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return {};
        if (end == path.npos)
            break;
        begin = end + 1;
    }
    std::string lower(path);
    for (auto& c : lower)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    if (!lower.ends_with(".fuz") && !lower.ends_with(".wav") && !lower.ends_with(".xwm"))
        return {};
    return path;
}

bool SamePath(std::string_view aLeft, std::string_view aRight)
{
    const auto trim = [](std::string_view aPath)
    {
        if (aPath.size() >= 11 && _strnicmp(aPath.data(), "Data\\Sound\\", 11) == 0)
            aPath.remove_prefix(11);
        else if (aPath.size() >= 6 && _strnicmp(aPath.data(), "Sound\\", 6) == 0)
            aPath.remove_prefix(6);
        return aPath;
    };
    aLeft = trim(aLeft);
    aRight = trim(aRight);
    return !aLeft.empty() && aLeft.size() == aRight.size() &&
        _strnicmp(aLeft.data(), aRight.data(), aLeft.size()) == 0;
}

Actor* RemoteActor(uint32_t aPlayerId)
{
    const auto& api = PluginMessagingService::GetInterface();
    if (aPlayerId == api.LocalPlayerId())
        return nullptr;
    const uint32_t form = api.ResolvePlayerFormId(aPlayerId);
    auto* actor = form ? Cast<Actor>(TESForm::GetById(form)) : nullptr;
    return actor && actor->GetExtension()->IsRemotePlayer() && actor->GetNiNode() ? actor : nullptr;
}
}

struct PlayerVoiceSync::State
{
    struct Peer { uint32_t Sequence{}, Form{}; bool Playing{}; };

    World& Game;
    uint32_t Session{}, Sequence{}, AdapterSequence{}, AdapterTopic{};
    std::string LastPath, PendingPath;
    bool LastWasAdapter{};
    Clock::time_point LastStart{}, PendingAt{}, NextPoll{};
    std::map<uint32_t, Peer> Peers;
    entt::scoped_connection UpdateConnection, LeaveConnection, DisconnectConnection;

    State(World& aWorld, entt::dispatcher& aDispatcher) : Game(aWorld)
    {
        PluginMessagingService::GetInterface().Register(kChannel, &Receive, this);
        UpdateConnection = aDispatcher.sink<UpdateEvent>().connect<&State::OnUpdate>(this);
        LeaveConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&State::OnLeave>(this);
        DisconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&State::OnDisconnect>(this);
    }

    ~State() { PluginMessagingService::GetInterface().Unregister(kChannel, &Receive); }

    void Reset(uint32_t aSession)
    {
        Session = aSession;
        Sequence = AdapterSequence = AdapterTopic = 0;
        LastPath.clear();
        PendingPath.clear();
        LastStart = PendingAt = {};
        LastWasAdapter = false;
        Peers.clear();
    }

    bool Send(uint8_t aType, uint32_t aSequence, std::string_view aPath = {})
    {
        std::vector<uint8_t> bytes(kHeaderSize);
        bytes[0] = aType;
        std::memcpy(bytes.data() + 1, &aSequence, sizeof(aSequence));
        if (!aPath.empty())
            bytes.insert(bytes.end(), aPath.begin(), aPath.end());
        return PluginMessagingService::GetInterface().Send(kChannel, SkyrimTogether::Nearby, bytes.data(), bytes.size()) ==
            SkyrimTogether::PluginMessageResult::Ok;
    }

    uint32_t Start(const std::string& acPath, bool aAdapter)
    {
        const auto now = Clock::now();
        // Deduplicate the native hook and a voice framework's report of its
        // same line, while still permitting repeated lines from one source.
        if (LastWasAdapter != aAdapter && !LastPath.empty() &&
            SamePath(LastPath, acPath) && now - LastStart < std::chrono::milliseconds(500))
            return Sequence;
        const uint32_t seq = Sequence + 1;
        if (!seq || !Send(kStart, seq, acPath))
            return 0;
        Sequence = seq;
        LastPath = acPath;
        LastStart = now;
        LastWasAdapter = aAdapter;
        spdlog::info("Player voice: sent line {} (SKSE adapter: {})", seq, aAdapter);
        return seq;
    }

    void OnUpdate(const UpdateEvent&)
    {
        const auto now = Clock::now();
        if (now < NextPoll)
            return;
        NextPoll = now + std::chrono::milliseconds(50);
        Game.QueueCheckpointTask([this]() { Tick(); });
    }

    void Native(std::string aPath)
    {
        Game.QueueCheckpointTask([this, path = std::move(aPath)]()
        {
            const uint32_t local = PluginMessagingService::GetInterface().LocalPlayerId();
            if (!local)
                return;
            if (Session != local)
                Reset(local);
            if (LastWasAdapter && !LastPath.empty() && SamePath(LastPath, path) &&
                Clock::now() - LastStart < std::chrono::milliseconds(500))
                return;
            if (!PendingPath.empty())
                Start(PendingPath, false);
            PendingPath = path;
            PendingAt = Clock::now() + std::chrono::milliseconds(100);
        });
    }

    void Tick()
    {
        const uint32_t local = PluginMessagingService::GetInterface().LocalPlayerId();
        const auto module = GetModuleHandleW(L"STCheckpointBridge.dll");
        const auto poll = module ? reinterpret_cast<STCheckpointBridge::PollVoiceEvent>(
            GetProcAddress(module, "STPollPlayerVoiceEvent")) : nullptr;
        const bool sameSession = local && Session == local;
        if (Session != local)
            Reset(local);

        if (poll)
        {
            STCheckpointBridge::VoiceEvent event{};
            for (unsigned int i = 0; i < 64 && poll(&event, sizeof(event)); ++i)
            {
                if (!sameSession)
                    continue;
                if (event.Type == STCheckpointBridge::VoiceStarted)
                {
                    const auto path = VoicePath(event.Path);
                    if (path.empty())
                        continue;
                    if (!PendingPath.empty() && SamePath(PendingPath, path))
                        PendingPath.clear();
                    AdapterTopic = event.Topic;
                    AdapterSequence = Start(path, true);
                }
                else if (event.Type == STCheckpointBridge::VoiceEnded &&
                         AdapterSequence && event.Topic == AdapterTopic)
                {
                    if (event.Reason != 0 && AdapterSequence == Sequence)
                        Send(kStop, AdapterSequence);
                    AdapterSequence = 0;
                }
            }
        }
        if (local && !PendingPath.empty() && Clock::now() >= PendingAt)
        {
            Start(PendingPath, false);
            PendingPath.clear();
        }
    }

    static void Receive(uint32_t aSender, const void* apData, size_t aSize, void* apContext)
    {
        auto* state = static_cast<State*>(apContext);
        if (!aSender || !apData || aSize < kHeaderSize || aSize > kHeaderSize + kMaxPath)
            return;
        const auto* pBytes = static_cast<const uint8_t*>(apData);
        const uint8_t type = pBytes[0];
        uint32_t seq{};
        std::memcpy(&seq, pBytes + 1, sizeof(seq));
        if (!seq || (type != kStart && type != kStop))
            return;
        std::string path;
        if (type == kStart)
        {
            path = VoicePath(std::string_view(reinterpret_cast<const char*>(pBytes + kHeaderSize), aSize - kHeaderSize));
            if (path.empty())
                return;
        }
        else if (aSize != kHeaderSize)
            return;

        const uint32_t local = PluginMessagingService::GetInterface().LocalPlayerId();
        if (!local || aSender == local)
            return;
        state->Game.QueueCheckpointTask([state, aSender, local, seq, type, path = std::move(path)]()
        {
            if (PluginMessagingService::GetInterface().LocalPlayerId() != local)
                return;
            if (state->Session != local)
                state->Reset(local);
            state->Apply(aSender, seq, type, path);
        });
    }

    void Apply(uint32_t aSender, uint32_t aSequence, uint8_t aType, const std::string& acPath)
    {
        const auto it = Peers.find(aSender);
        if (aType == kStop)
        {
            if (it != Peers.end() && it->second.Playing && it->second.Sequence == aSequence)
            {
                auto* actor = RemoteActor(aSender);
                if (actor && actor->formID == it->second.Form)
                    actor->StopCurrentDialogue(true);
                it->second.Playing = false;
            }
            return;
        }
        if (it != Peers.end() && aSequence <= it->second.Sequence)
            return;
        auto* actor = RemoteActor(aSender);
        if (!actor || (it == Peers.end() && Peers.size() >= 128))
            return;
        actor->StopCurrentDialogue(true);
        // Same file-backed playback as NPC dialogue, bypassing the local hook.
        actor->SpeakSound(acPath.c_str());
        Peers[aSender] = Peer{aSequence, actor->formID, true};
        spdlog::info("Player voice: replayed line {} from player {}", aSequence, aSender);
    }

    void OnLeave(const NotifyPlayerLeft& acMessage)
    {
        const uint32_t id = acMessage.PlayerId;
        Game.QueueCheckpointTask([this, id]() { Peers.erase(id); });
    }

    void OnDisconnect(const DisconnectedEvent&)
    {
        Game.QueueCheckpointTask([this]() { Reset(0); });
    }
};

PlayerVoiceSync::PlayerVoiceSync(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_state(TiltedPhoques::MakeUnique<State>(aWorld, aDispatcher))
{
}

PlayerVoiceSync::~PlayerVoiceSync() = default;

void PlayerVoiceSync::CaptureNative(const char* apPath) noexcept
{
    if (!apPath)
        return;
    auto path = VoicePath(apPath);
    if (!path.empty())
        m_state->Native(std::move(path));
}
