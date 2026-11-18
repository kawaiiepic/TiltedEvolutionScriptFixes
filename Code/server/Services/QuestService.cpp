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
Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "Share miscellaneous and hidden quests by default", false, Console::SettingsFlags::kLocked};

constexpr const char* kJournalPath = "shared-quests.log";
constexpr const char* kJournalBackupPath = "shared-quests.log.backup";
constexpr const char* kPoliciesPath = "shared-quest-policies.conf";
constexpr auto kSceneObserverTimeout = std::chrono::seconds(20);
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
    , m_policies(kPoliciesPath)
{
    if (m_journal.IsHealthy())
        spdlog::info("Loaded {} shared quest records", m_journal.Records().size());
    else
        spdlog::error("Quest sync disabled: unable to load {} (error line {}).", kJournalPath, m_journal.ErrorLine());
    if (!m_policies.IsHealthy())
        spdlog::error("Quest and scene sync disabled: unable to load {} (error line {}).", kPoliciesPath, m_policies.ErrorLine());

    m_sceneConnection = aDispatcher.sink<PacketEvent<RequestSceneState>>().connect<&QuestService::OnSceneState>(this);
    m_sceneTickConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnSceneTick>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&QuestService::OnPlayerLeave>(this);
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_questSyncTickConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnQuestSyncTick>(this);
    m_questSyncAckConnection = aDispatcher.sink<PacketEvent<RequestQuestSyncAck>>().connect<&QuestService::OnQuestSyncAck>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&QuestService::OnPlayerJoin>(this);
}

void QuestService::ShowCampaignStatus() const noexcept
{
    auto pOut = spdlog::get("ConOut");
    pOut->info("Quest journal {}: {}, {} committed records, error line {}", kJournalPath,
               m_journal.IsHealthy() ? "healthy" : "unavailable", m_journal.Records().size(), m_journal.ErrorLine());
    pOut->info("Quest policies {}: {}, {} overrides, error line {}", kPoliciesPath,
               m_policies.IsHealthy() ? "healthy" : "unavailable", m_policies.Size(), m_policies.ErrorLine());
    for (const auto& [playerId, sync] : m_questSync)
    {
        pOut->info("Player {} replay cursor {}/{}, {} awaiting acknowledgement", playerId, sync.Cursor,
                   m_journal.Records().size(), sync.Pending.Updates.size());
    }
}

void QuestService::BackupCampaign() const noexcept
{
    auto pOut = spdlog::get("ConOut");
    if (!m_journal.IsHealthy() || m_journal.Records().empty())
    {
        pOut->warn("A healthy journal with committed records is required for backup.");
        return;
    }

    // The console and quest handlers run on the server thread, so appends
    // cannot interleave with the copy. Preserve any previous backup.
    std::error_code error;
    if (std::filesystem::copy_file(kJournalPath, kJournalBackupPath, error))
        pOut->info("Backed up {} shared quest records to {}.", m_journal.Records().size(), kJournalBackupPath);
    else
        pOut->error("Unable to back up {} to {}: {}", kJournalPath, kJournalBackupPath, error.message());
}

const Campaign::QuestSnapshot* QuestService::GetCampaignQuest(const Campaign::PluginFormKey& acKey) const noexcept
{
    return m_journal.Find(acKey);
}

void QuestService::QueueEntry(NotifyQuestUpdate& aBatch, const Campaign::JournalRecord& acRecord, const Player& acPlayer) const noexcept
{
    const auto& event = acRecord.Event;
    if (acRecord.Origin == acPlayer.GetId() || !m_policies.IsShared(event.Quest, IsSharedType(event.Type)))
        return;

    // Network mod IDs are assigned on connection and can change after a restart.
    const auto& mods = m_world.ctx().at<ModsComponent>();
    const auto id = mods.FindFormId(event.Quest);
    if (!id || !acPlayer.HasModId(id->ModId))
        return;
    if (acRecord.ApplyStageBeforeStop)
        aBatch.Updates.push_back({*id, event.Stage, SharedQuestUpdate::StageUpdate});
    aBatch.Updates.push_back({*id, event.Stage, event.Status});
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
        if (!scene.Ended)
            SendScene(scene.Canonical, *acEvent.pPlayer);
    }
    if (m_journal.IsHealthy() && m_policies.IsHealthy())
        m_questSync.try_emplace(acEvent.pPlayer->GetId());
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (message.ClientQuestType > 11 || message.Status > RequestQuestUpdate::Stopped)
        return;

    Campaign::QuestEvent entry{{}, message.Stage, static_cast<Campaign::QuestEvent::Kind>(message.Status), message.ClientQuestType};
    const auto key = m_world.ctx().at<ModsComponent>().FindFormKey(message.Id, *acMessage.pPlayer);
    if (!key)
        return;
    entry.Quest = *key;

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

    if (!m_journal.IsHealthy() || !m_policies.IsShared(entry.Quest, IsSharedType(entry.Type)))
        return;

    const auto result = m_journal.Append(entry, acMessage.pPlayer->GetId());
    if (result == Campaign::QuestJournal::Result::Unavailable)
        spdlog::error("Quest sync writes disabled: unable to commit record {} to {}.", m_journal.ErrorLine(), kJournalPath);
    else if (result == Campaign::QuestJournal::Result::Committed)
    {
        spdlog::debug("Shared quest {}:{:X} stage {} status {}", entry.Quest.Plugin, entry.Quest.BaseId, entry.Stage, entry.Status);
        m_world.GetScriptService().HandleCampaignQuestCommitted(m_journal.Records().size(), entry, acMessage.pPlayer->GetId());
    }
}

void QuestService::SendScene(const NotifySceneState& acState, const Player& acPlayer) noexcept
{
    if (!acPlayer.HasModId(acState.State.Id.ModId) || !acPlayer.HasModId(acState.State.Quest.ModId))
        return;

    const auto missingPlugin = [&](const auto& alias) { return !acPlayer.HasModId(alias.Reference.ModId); };
    if (!std::any_of(acState.State.ReferenceAliases.begin(), acState.State.ReferenceAliases.end(), missingPlugin))
    {
        acPlayer.Send(acState);
        return;
    }

    auto filtered = acState;
    std::erase_if(filtered.State.ReferenceAliases, missingPlugin);
    acPlayer.Send(filtered);
}

void QuestService::BroadcastScene(const NotifySceneState& acState) noexcept
{
    for (const Player* pPlayer : m_world.GetPlayerManager())
        SendScene(acState, *pPlayer);
}

void QuestService::RemoveSceneObserver(SharedScene& aScene, uint32_t aPlayerId, bool aFinished) noexcept
{
    if (!aScene.Observers.erase(aPlayerId) || aScene.Ended || aScene.Canonical.Owner != aPlayerId)
        return;

    if (!aFinished)
    {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(aScene.Observers, [&](const auto& observer)
        {
            return now - observer.second.LastSeen > kSceneObserverTimeout;
        });
    }

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

    const auto quest = m_world.ctx().at<ModsComponent>().FindFormKey(state.Quest, *pPlayer);
    if (!quest || !m_policies.IsShared(*quest, true))
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
            if (now - observer.LastSeen > kSceneObserverTimeout)
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
