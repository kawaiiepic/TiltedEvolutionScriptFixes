#include <TiltedOnlinePCH.h>

#include <ModCompat/RaceMenuSync.h>
#include <ModCompat/RaceMenuVisuals.h>
#include <Services/PluginMessagingService.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Games/ActorExtension.h>
#include <Interface/UI.h>
#include <PlayerCharacter.h>
#include <World.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
// Public RaceMenu SKEE interface ABI. Only slots through UpdateModelWeight are used.
struct SkeeInterface
{
    virtual ~SkeeInterface() = default;
    virtual uint32_t GetVersion() = 0;
    virtual void Revert() = 0;
};

struct SkeeInterfaceMap
{
    virtual SkeeInterface* QueryInterface(const char*) = 0;
};

struct SkeeBodyMorph : SkeeInterface
{
    struct NameVisitor { virtual void Visit(TESObjectREFR*, const char*) = 0; };
    struct KeyVisitor { virtual void Visit(const char*, float) = 0; };
    struct ValueVisitor { virtual void Visit(TESObjectREFR*, const char*, const char*, float) = 0; };

    virtual void SetMorph(TESObjectREFR*, const char*, const char*, float) = 0;
    virtual float GetMorph(TESObjectREFR*, const char*, const char*) = 0;
    virtual void ClearMorph(TESObjectREFR*, const char*, const char*) = 0;
    virtual float GetBodyMorphs(TESObjectREFR*, const char*) = 0;
    virtual void ClearBodyMorphNames(TESObjectREFR*, const char*) = 0;
    virtual void VisitMorphs(TESObjectREFR*, NameVisitor&) = 0;
    virtual void VisitKeys(TESObjectREFR*, const char*, KeyVisitor&) = 0;
    virtual void VisitMorphValues(TESObjectREFR*, ValueVisitor&) = 0;
    virtual void ClearMorphs(TESObjectREFR*) = 0;
    virtual void ApplyVertexDiff(TESObjectREFR*, NiAVObject*, bool) = 0;
    virtual void ApplyBodyMorphs(TESObjectREFR*, bool) = 0;
    virtual void UpdateModelWeight(TESObjectREFR*, bool) = 0;
};

constexpr char kChannel[] = "skyrimtogether.racemenu.appearance.v1";
constexpr char kMorphKey[] = "SkyrimTogether";
constexpr size_t kMaxName = 80;
constexpr size_t kMaxSnapshot = 24 * 1024;
constexpr size_t kChunkSize = 4000;
constexpr size_t kMaxPeers = 64;
using Morphs = std::map<std::string, float>;

bool ValidName(std::string_view aName)
{
    return !aName.empty() && aName.size() <= kMaxName &&
           aName.find_first_of("\r\n") == aName.npos && aName.find('\0') == aName.npos;
}

bool ValidValue(float aValue)
{
    return std::isfinite(aValue) && std::abs(aValue) <= 10.f;
}

struct Names final : SkeeBodyMorph::NameVisitor
{
    std::set<std::string> Values;
    void Visit(TESObjectREFR*, const char* apName) override
    {
        if (apName && ValidName(apName) && Values.size() < 512)
            Values.emplace(apName);
    }
};

void Append(std::vector<uint8_t>& aBuffer, const void* apData, size_t aSize)
{
    const auto* pData = static_cast<const uint8_t*>(apData);
    aBuffer.insert(aBuffer.end(), pData, pData + aSize);
}

struct Header
{
    uint32_t Revision;
    uint16_t Part;
    uint16_t Count;
};
static_assert(sizeof(Header) == 8);

bool Decode(const std::vector<uint8_t>& acBytes, Morphs& aMorphs, RaceMenuVisuals::Snapshot& aVisuals)
{
    uint16_t morphSize{};
    if (acBytes.size() < sizeof(morphSize))
        return false;
    std::memcpy(&morphSize, acBytes.data(), sizeof(morphSize));
    size_t at = sizeof(morphSize);
    const size_t end = at + morphSize;
    if (end > acBytes.size())
        return false;
    while (at < end)
    {
        const size_t length = acBytes[at++];
        if (length > end - at || end - at - length < sizeof(float))
            return false;
        std::string name(reinterpret_cast<const char*>(acBytes.data() + at), length);
        at += length;
        float value;
        std::memcpy(&value, acBytes.data() + at, sizeof(value));
        at += sizeof(value);
        if (!ValidName(name) || !ValidValue(value) || !aMorphs.emplace(std::move(name), value).second)
            return false;
    }
    return RaceMenuVisuals::Decode(acBytes.data() + end, acBytes.size() - end, aVisuals);
}
}

struct RaceMenuSync::State
{
    struct Peer
    {
        uint32_t Revision{};
        uint16_t NextPart{};
        uint16_t PartCount{};
        std::vector<uint8_t> Bytes;
        Morphs Desired;
        Morphs Applied;
        RaceMenuVisuals::Snapshot DesiredVisuals, AppliedVisuals;
        uint32_t Form{};
        const NiNode* pNode{};
        bool Ready{};
    };

    World& Game;
    SkeeBodyMorph* pMorph{};
    std::map<uint32_t, Peer> Peers;
    std::vector<uint8_t> LastSent;
    uint32_t Revision{};
    std::chrono::steady_clock::time_point NextUpdate{}, ResendAt{};
    entt::scoped_connection UpdateConnection, LeaveConnection, DisconnectConnection;

    State(World& aWorld, entt::dispatcher& aDispatcher) : Game(aWorld)
    {
        PluginMessagingService::GetInterface().Register(kChannel, &Receive, this);
        UpdateConnection = aDispatcher.sink<UpdateEvent>().connect<&State::OnUpdate>(this);
        LeaveConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&State::OnLeave>(this);
        DisconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&State::OnDisconnect>(this);
    }

    ~State() { PluginMessagingService::GetInterface().Unregister(kChannel, &Receive); }

    static void Receive(uint32_t aSender, const void* apData, size_t aSize, void* apContext)
    {
        auto& state = *static_cast<State*>(apContext);
        if (!apData || aSize < sizeof(Header) || aSize > sizeof(Header) + kChunkSize)
            return;
        Header header;
        std::memcpy(&header, apData, sizeof(header));
        if (!header.Revision || !header.Count || header.Count > (kMaxSnapshot + kChunkSize - 1) / kChunkSize ||
            header.Part >= header.Count || (!state.Peers.contains(aSender) && state.Peers.size() >= kMaxPeers))
            return;

        auto& peer = state.Peers[aSender];
        if (!header.Part && header.Revision > peer.Revision)
        {
            peer.Revision = header.Revision;
            peer.PartCount = header.Count;
            peer.NextPart = 0;
            peer.Bytes.clear();
        }

        const size_t length = aSize - sizeof(Header);
        if (header.Revision != peer.Revision || header.Count != peer.PartCount || header.Part != peer.NextPart ||
            peer.Bytes.size() + length > kMaxSnapshot)
            return;
        if (length)
            Append(peer.Bytes, static_cast<const uint8_t*>(apData) + sizeof(Header), length);
        if (++peer.NextPart != peer.PartCount)
            return;

        Morphs snapshot;
        RaceMenuVisuals::Snapshot visuals;
        if (Decode(peer.Bytes, snapshot, visuals))
        {
            peer.Desired = std::move(snapshot);
            peer.DesiredVisuals = std::move(visuals);
            peer.Ready = true;
        }
        peer.Bytes.clear();
    }

    void OnUpdate(const UpdateEvent&)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!Game.GetTransport().IsOnline() || now < NextUpdate)
            return;
        NextUpdate = now + std::chrono::seconds(1);
        Game.QueueCheckpointTask([this]() { Tick(); });
    }

    void OnLeave(const NotifyPlayerLeft& acMessage)
    {
        const uint32_t id = acMessage.PlayerId;
        Game.QueueCheckpointTask([this, id]()
        {
            if (const auto it = Peers.find(id); it != Peers.end())
            {
                Clear(it->second);
                Peers.erase(it);
            }
        });
    }

    void OnDisconnect(const DisconnectedEvent&)
    {
        Game.QueueCheckpointTask([this]()
        {
            for (auto& [id, peer] : Peers)
                Clear(peer);
            Peers.clear();
            LastSent.clear();
            ResendAt = {};
        });
    }

    void Clear(const Peer& acPeer)
    {
        auto* pActor = acPeer.Form ? Cast<Actor>(TESForm::GetById(acPeer.Form)) : nullptr;
        if (!pMorph || !pActor || !pActor->GetExtension()->IsRemotePlayer())
            return;
        for (const auto& [name, value] : acPeer.Applied)
            pMorph->ClearMorph(pActor, name.c_str(), kMorphKey);
        if (pActor->GetNiNode())
        {
            if (!acPeer.Applied.empty())
                pMorph->ApplyBodyMorphs(pActor, false);
            RaceMenuVisuals::Apply(*pActor, acPeer.AppliedVisuals, {});
        }
    }

    void Tick()
    {
        if (!Game.GetTransport().IsOnline())
            return;
        auto* pUI = UI::Get();
        auto* pPlayer = PlayerCharacter::Get();
        if (!pUI || !pPlayer || !pPlayer->GetNiNode() || pUI->GetMenuOpen("Loading Menu") ||
            pUI->GetMenuOpen("RaceSex Menu"))
            return;

        if (!pMorph)
        {
            const auto module = GetModuleHandleW(L"STCheckpointBridge.dll");
            using Query = SkeeInterfaceMap* (*)() noexcept;
            const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "STQueryRaceMenuInterfaces")) : nullptr;
            auto* pInterfaces = query ? query() : nullptr;
            auto* pBody = pInterfaces ? pInterfaces->QueryInterface("BodyMorph") : nullptr;
            if (!pBody || pBody->GetVersion() != 5)
                return;
            pMorph = static_cast<SkeeBodyMorph*>(pBody);
            RaceMenuVisuals::Initialize(pInterfaces);
        }

        Broadcast(*pPlayer);
        const auto& api = PluginMessagingService::GetInterface();
        for (auto& [id, peer] : Peers)
        {
            if (!peer.Ready)
                continue;
            const uint32_t form = api.ResolvePlayerFormId(id);
            auto* pActor = form ? Cast<Actor>(TESForm::GetById(form)) : nullptr;
            const auto* pNode = pActor ? pActor->GetNiNode() : nullptr;
            if (!pNode)
            {
                peer.pNode = nullptr;
                continue;
            }
            if (peer.Form != form)
            {
                Clear(peer);
                peer.Applied.clear();
                peer.AppliedVisuals = {};
                peer.Form = form;
            }

            bool changed = peer.pNode != pNode && (!peer.Desired.empty() || !peer.Applied.empty());
            for (const auto& [name, value] : peer.Applied)
                if (!peer.Desired.contains(name))
                {
                    pMorph->ClearMorph(pActor, name.c_str(), kMorphKey);
                    changed = true;
                }
            for (const auto& [name, value] : peer.Desired)
                if (pMorph->GetMorph(pActor, name.c_str(), kMorphKey) != value)
                {
                    pMorph->SetMorph(pActor, name.c_str(), kMorphKey, value);
                    changed = true;
                }
            if (changed)
            {
                pMorph->ApplyBodyMorphs(pActor, false);
                pMorph->UpdateModelWeight(pActor, false);
            }
            if (peer.Applied != peer.Desired)
                peer.Applied = peer.Desired;
            if (peer.pNode != pNode || peer.AppliedVisuals != peer.DesiredVisuals)
            {
                RaceMenuVisuals::Apply(*pActor, peer.AppliedVisuals, peer.DesiredVisuals);
                peer.AppliedVisuals = peer.DesiredVisuals;
            }
            peer.pNode = pNode;
        }
    }

    void Broadcast(PlayerCharacter& aPlayer)
    {
        Names names;
        pMorph->VisitMorphs(&aPlayer, names);
        auto visuals = RaceMenuVisuals::Encode(RaceMenuVisuals::Capture(aPlayer));
        std::vector<uint8_t> bytes(sizeof(uint16_t));
        for (const auto& name : names.Values)
        {
            const float value = pMorph->GetBodyMorphs(&aPlayer, name.c_str());
            if (!ValidValue(value) || value == 0.f)
                continue;
            if (bytes.size() + 1 + name.size() + sizeof(value) + visuals.size() > kMaxSnapshot)
                break;
            bytes.push_back(static_cast<uint8_t>(name.size()));
            Append(bytes, name.data(), name.size());
            Append(bytes, &value, sizeof(value));
        }

        const auto morphSize = static_cast<uint16_t>(bytes.size() - sizeof(uint16_t));
        std::memcpy(bytes.data(), &morphSize, sizeof(morphSize));
        bytes.insert(bytes.end(), visuals.begin(), visuals.end());

        const auto now = std::chrono::steady_clock::now();
        if (bytes == LastSent && now < ResendAt)
            return;

        const auto count = static_cast<uint16_t>(std::max<size_t>(1, (bytes.size() + kChunkSize - 1) / kChunkSize));
        const auto revision = ++Revision;
        const auto& api = PluginMessagingService::GetInterface();
        for (uint16_t part = 0; part < count; ++part)
        {
            const Header header{revision, part, count};
            std::vector<uint8_t> packet;
            Append(packet, &header, sizeof(header));
            const size_t offset = part * kChunkSize;
            const size_t length = std::min(kChunkSize, bytes.size() - offset);
            if (length)
                Append(packet, bytes.data() + offset, length);
            if (api.Send(kChannel, SkyrimTogether::Broadcast, packet.data(), packet.size()) != SkyrimTogether::PluginMessageResult::Ok)
                return;
        }
        LastSent = std::move(bytes);
        ResendAt = now + std::chrono::seconds(10);
    }
};

RaceMenuSync::RaceMenuSync(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_state(TiltedPhoques::MakeUnique<State>(aWorld, aDispatcher))
{
}

RaceMenuSync::~RaceMenuSync() = default;
