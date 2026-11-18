#include <GameServer.h>
#include <Components.h>
#include <World.h>
#include <Services/QuestService.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestSceneState.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/RequestQuestSyncAck.h>
#include <Setting.h>

#include <algorithm>
#include <iterator>
#include <set>

namespace
{
Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "(Experimental) Share miscellaneous and hidden quests", false};

constexpr const char* kJournalPath = "shared-quests.log";
static_assert(Campaign::QuestEvent::StageUpdate == static_cast<int>(RequestQuestUpdate::StageUpdate) &&
              Campaign::QuestEvent::Started == static_cast<int>(RequestQuestUpdate::Started) &&
              Campaign::QuestEvent::Stopped == static_cast<int>(RequestQuestUpdate::Stopped));
static_assert(Campaign::QuestEvent::StageUpdate == static_cast<int>(SharedQuestUpdate::StageUpdate) &&
              Campaign::QuestEvent::Started == static_cast<int>(SharedQuestUpdate::Started) &&
              Campaign::QuestEvent::Stopped == static_cast<int>(SharedQuestUpdate::Stopped));

bool IsSharedType(uint8_t aType) noexcept
{
    return aType <= 11 && ((aType != 0 && aType != 6) || bEnableMiscQuestSync);
}
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_journal(kJournalPath)
{
    if (m_journal.IsHealthy())
        spdlog::info("Loaded {} shared quest records", m_journal.Records().size());
    else
        spdlog::error("Quest sync disabled: unable to load {} (error line {}).", kJournalPath, m_journal.ErrorLine());
    m_sceneConnection = aDispatcher.sink<PacketEvent<RequestSceneState>>().connect<&QuestService::OnSceneState>(this);
    m_sceneTickConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnSceneTick>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&QuestService::OnPlayerLeave>(this);
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_questSyncTickConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnQuestSyncTick>(this);
    m_questSyncAckConnection = aDispatcher.sink<PacketEvent<RequestQuestSyncAck>>().connect<&QuestService::OnQuestSyncAck>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&QuestService::OnPlayerJoin>(this);
}

bool QuestService::ResolveEntry(const RequestQuestUpdate& acRequest, const Player& acPlayer, Campaign::QuestEvent& aEntry) const noexcept
{
    if (!acPlayer.HasModId(acRequest.Id.ModId) || acRequest.Id.BaseId == 0)
        return false;

    const auto& mods = m_world.ctx().at<ModsComponent>();
    for (bool lite : {false, true})
    {
        const auto& list = lite ? mods.GetLiteMods() : mods.GetStandardMods();
        for (const auto& [name, mod] : list)
        {
            if (mod.id != acRequest.Id.ModId)
                continue;
            if (acRequest.Id.BaseId > (lite ? 0xFFFu : 0xFFFFFFu))
                return false;

            aEntry = {{name.c_str(), lite, acRequest.Id.BaseId}, acRequest.Stage,
                      static_cast<Campaign::QuestEvent::Kind>(acRequest.Status), acRequest.ClientQuestType};
            return true;
        }
    }
    return false;
}

void QuestService::QueueEntry(NotifyQuestUpdate& aBatch, const Campaign::JournalRecord& acRecord, const Player& acPlayer) const noexcept
{
    const auto& event = acRecord.Event;
    if (acRecord.Origin == acPlayer.GetId() || !IsSharedType(event.Type))
        return;

    // Network mod IDs are assigned on connection and can change after a restart.
    const auto& mods = m_world.ctx().at<ModsComponent>();
    const auto& list = event.Quest.Lite ? mods.GetLiteMods() : mods.GetStandardMods();
    const auto it = list.find(String(event.Quest.Plugin.c_str()));
    if (it == list.end() || !acPlayer.HasModId(it->second.id))
        return;

    const GameId id(it->second.id, event.Quest.BaseId);
    if (acRecord.ApplyStageBeforeStop)
        aBatch.Updates.push_back({id, event.Stage, SharedQuestUpdate::StageUpdate});
    aBatch.Updates.push_back({id, event.Stage, event.Status});
}

void QuestService::OnQuestSyncTick(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    const auto& records = m_journal.Records();
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        auto it = m_questSync.find(pPlayer->GetId());
        if (it == m_questSync.end())
            continue;
        auto& sync = it->second;
        if (sync.Pending.Updates.empty())
        {
            const size_t cScanLimit = sync.Cursor + std::min<size_t>(128, records.size() - sync.Cursor);
            while (sync.Cursor < cScanLimit && sync.Pending.Updates.size() + 2 <= NotifyQuestUpdate::kMaxUpdates)
            {
                QueueEntry(sync.Pending, records[sync.Cursor], *pPlayer);
                ++sync.Cursor;
            }
            if (sync.Pending.Updates.empty())
                continue;
            sync.Pending.Through = sync.Cursor;
            sync.RetryAt = now;
        }

        if (now >= sync.RetryAt)
        {
            pPlayer->Send(sync.Pending);
            sync.RetryAt = now + std::chrono::seconds(2);
        }
    }
}

void QuestService::OnQuestSyncAck(const PacketEvent<RequestQuestSyncAck>& acEvent) noexcept
{
    const auto it = m_questSync.find(acEvent.pPlayer->GetId());
    if (it != m_questSync.end() && it->second.Pending.Through == acEvent.Packet.Through)
        it->second.Pending.Updates.clear();
}

void QuestService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    for (const auto& [id, scene] : m_scenes)
    {
        const auto& state = scene.Canonical;
        if (!scene.Ended && acEvent.pPlayer->HasModId(state.State.Id.ModId) && acEvent.pPlayer->HasModId(state.State.Quest.ModId))
            acEvent.pPlayer->Send(state);
    }
    if (m_journal.IsHealthy())
        m_questSync.try_emplace(acEvent.pPlayer->GetId());
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (!IsSharedType(message.ClientQuestType) || message.Status > RequestQuestUpdate::Stopped)
        return;

    Campaign::QuestEvent entry;
    if (!ResolveEntry(message, *acMessage.pPlayer, entry))
        return;

    // Preserve the existing per-player active quest log for other services, independently
    // of the campaign's historical journal and its duplicate suppression.
    auto& entries = acMessage.pPlayer->GetQuestLogComponent().QuestContent.Entries;
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.Id == message.Id; });
    if (message.Status == RequestQuestUpdate::Stopped)
    {
        if (it != entries.end())
            entries.erase(it);
    }
    else if (it == entries.end())
    {
        auto& record = entries.emplace_back();
        record.Id = message.Id;
        record.Stage = message.Stage;
    }
    else
        it->Stage = std::max(it->Stage, message.Stage);

    if (!m_journal.IsHealthy())
        return;

    const auto result = m_journal.Append(entry, acMessage.pPlayer->GetId());
    if (result == Campaign::QuestJournal::Result::Unavailable)
        spdlog::error("Quest sync writes disabled: unable to commit record {} to {}.", m_journal.ErrorLine(), kJournalPath);
    else if (result == Campaign::QuestJournal::Result::Committed)
        spdlog::debug("Shared quest {}:{:X} stage {} status {}", entry.Quest.Plugin, entry.Quest.BaseId, entry.Stage, entry.Status);
}

void QuestService::BroadcastScene(const NotifySceneState& acState) noexcept
{
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->HasModId(acState.State.Id.ModId) && pPlayer->HasModId(acState.State.Quest.ModId))
            pPlayer->Send(acState);
    }
}

void QuestService::RemoveSceneObserver(SharedScene& aScene, uint32_t aPlayerId, bool aFinished) noexcept
{
    aScene.Observers.erase(aPlayerId);
    if (aScene.Ended || aScene.Canonical.Owner != aPlayerId)
        return;

    if (!aFinished && !aScene.Observers.empty())
    {
        // Transfer only to a client actually observing the same native scene.
        // Prefer the observer furthest along, while preserving the generation.
        auto best = aScene.Observers.begin();
        for (auto it = std::next(best); it != aScene.Observers.end(); ++it)
        {
            const uint32_t cPhase = it->second.State.Phase;
            if (cPhase != UINT32_MAX && (best->second.State.Phase == UINT32_MAX || cPhase > best->second.State.Phase))
                best = it;
        }
        aScene.Canonical.Owner = best->first;
        aScene.Canonical.State = best->second.State;
        BroadcastScene(aScene.Canonical);
        return;
    }

    aScene.Ended = true;
    aScene.Canonical.State.Active = false;
    BroadcastScene(aScene.Canonical);
}

void QuestService::OnSceneState(const PacketEvent<RequestSceneState>& acEvent) noexcept
{
    const auto& state = acEvent.Packet.State;
    const auto* pPlayer = acEvent.pPlayer;
    if (!state.Id.BaseId || !state.Quest.BaseId ||
        !pPlayer->HasModId(state.Id.ModId) || !pPlayer->HasModId(state.Quest.ModId))
        return;

    // Reference aliases share the existing scene owner's snapshot. Validate
    // plugin mappings and unique alias IDs before storing or relaying them.
    std::set<uint32_t> aliasIds;
    for (const auto& alias : state.ReferenceAliases)
    {
        if (!alias.Reference.BaseId || !pPlayer->HasModId(alias.Reference.ModId) ||
            !aliasIds.insert(alias.AliasId).second)
            return;
    }

    const uint64_t cKey = state.Id.LogFormat();
    auto it = m_scenes.find(cKey);
    if (!state.Active)
    {
        if (it != m_scenes.end() && it->second.Canonical.State.Quest == state.Quest)
        {
            RemoveSceneObserver(it->second, pPlayer->GetId(), true);
            if (it->second.Observers.empty())
                m_scenes.erase(it);
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (it == m_scenes.end())
    {
        auto& scene = m_scenes[cKey];
        scene.Canonical.Owner = pPlayer->GetId();
        if (++m_nextSceneGeneration == 0)
            ++m_nextSceneGeneration;
        scene.Canonical.Generation = m_nextSceneGeneration;
        scene.Canonical.State = state;
        scene.Observers.emplace(pPlayer->GetId(), SceneObserver{state, now});
        BroadcastScene(scene.Canonical);
        return;
    }

    auto& scene = it->second;
    if (scene.Canonical.State.Quest != state.Quest)
        return;
    if (!scene.Ended && scene.Canonical.Owner != pPlayer->GetId())
    {
        const auto old = scene.Observers.find(pPlayer->GetId());
        if (old == scene.Observers.end() || old->second.State.ReferenceAliases != state.ReferenceAliases)
        {
            for (const auto& actual : state.ReferenceAliases)
            {
                const auto& aliases = scene.Canonical.State.ReferenceAliases;
                const auto expected = std::find_if(aliases.begin(), aliases.end(),
                    [&](const auto& a) { return a.AliasId == actual.AliasId; });
                if (expected != aliases.end() && expected->Reference != actual.Reference)
                    spdlog::warn("Player {} has divergent scene {:X} alias {}: expected {:X}, observed {:X}",
                        pPlayer->GetId(), cKey, actual.AliasId, expected->Reference.LogFormat(), actual.Reference.LogFormat());
            }
        }
    }
    scene.Observers[pPlayer->GetId()] = {state, now};
    // Once the owner finishes, lagging followers cannot open a new instance
    // until the observers have all stopped or disconnected.
    if (scene.Ended || scene.Canonical.Owner != pPlayer->GetId())
        return;

    if (scene.Canonical.State != state)
    {
        scene.Canonical.State = state;
        BroadcastScene(scene.Canonical);
    }
}

void QuestService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    m_questSync.erase(acEvent.pPlayer->GetId());
    for (auto it = m_scenes.begin(); it != m_scenes.end();)
    {
        RemoveSceneObserver(it->second, acEvent.pPlayer->GetId(), false);
        if (it->second.Observers.empty())
            it = m_scenes.erase(it);
        else
            ++it;
    }
}

void QuestService::OnSceneTick(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextSceneTick)
        return;
    m_nextSceneTick = now + std::chrono::seconds(2);

    for (auto it = m_scenes.begin(); it != m_scenes.end();)
    {
        auto& scene = it->second;
        std::vector<uint32_t> expired;
        for (const auto& [playerId, observer] : scene.Observers)
        {
            if (now - observer.LastSeen > std::chrono::seconds(20))
                expired.push_back(playerId);
        }
        // Remove expired followers before migrating an expired owner.
        for (uint32_t playerId : expired)
            if (playerId != scene.Canonical.Owner)
                RemoveSceneObserver(scene, playerId, false);
        if (std::find(expired.begin(), expired.end(), scene.Canonical.Owner) != expired.end())
            RemoveSceneObserver(scene, scene.Canonical.Owner, false);

        if (scene.Observers.empty())
            it = m_scenes.erase(it);
        else
            ++it;
    }
}
