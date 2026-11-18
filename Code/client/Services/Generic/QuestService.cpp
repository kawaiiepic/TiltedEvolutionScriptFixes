#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>

#include <Services/QuestService.h>
#include <Services/TransportService.h>

#include <PlayerCharacter.h>
#include <Forms/TESQuest.h>
#include <Games/TES.h>
#include <Games/Overrides.h>

#include <Events/EventDispatcher.h>

#include <Messages/RequestQuestUpdate.h>
#include <Messages/RequestQuestSyncAck.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/RequestSceneState.h>

static TESQuest* FindQuestByNameId(const String& name)
{
    auto& questRegistry = ModManager::Get()->quests;
    auto it = std::find_if(questRegistry.begin(), questRegistry.end(), [name](auto* it) { return std::strcmp(it->idName.AsAscii(), name.c_str()); });

    return it != questRegistry.end() ? *it : nullptr;
}

using HeldStages = QuestService::HeldStages;

static void ReleaseStagesForWaitingPhases(TESQuest* apQuest, HeldStages& aHeld) noexcept;
static void ReleaseUncheckedStages(TESQuest* apQuest, HeldStages& aHeld) noexcept;

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_joinedConnection = aDispatcher.sink<ConnectedEvent>().connect<&QuestService::OnConnected>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&QuestService::OnUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&QuestService::OnDisconnected>(this);
    m_sceneConnection = aDispatcher.sink<NotifySceneState>().connect<&QuestService::OnSceneState>(this);
    m_questUpdateConnection = aDispatcher.sink<NotifyQuestUpdate>().connect<&QuestService::OnQuestUpdate>(this);

    // Game events:
    // Starts, stops and stages are sent from TESQuestStartStopEvent and TESQuestStageEvent, which fire while the quest
    // changes. Shared progression is independent of party membership.
    // TESQuestStageItemDoneEvent fires once for each of a stage's log entries, after its fragment has run. That's too
    // late to send on, but it tells when an applied held stage is done.
    // The scene, scene action, phase and dialogue line events are only logged, to follow scenes when debugging held
    // stages. OnUpdate explains why they can't trigger the release of held stages.

    auto* pEventList = EventDispatcherManager::Get();
    pEventList->questStartStopEvent.RegisterSink(this);
    pEventList->questStageEvent.RegisterSink(this);
    pEventList->questStageItemDoneEvent.RegisterSink(this);
    pEventList->sceneEvent.RegisterSink(this);
    pEventList->sceneActionEvent.RegisterSink(this);
    pEventList->scenePhaseEvent.RegisterSink(this);
    pEventList->topicInfoEvent.RegisterSink(this);
}

void QuestService::OnConnected(const ConnectedEvent&) noexcept
{
    m_heldStages.clear();
    m_pendingUpdates.clear();
    m_receivedThrough = 0;
    m_pendingStops.clear();
    m_localScenes.clear();
    m_sharedScenes.clear();
}

void QuestService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_world.GetTransport().IsOnline())
        return;

    RefreshSceneStates();

    // Apply a bounded amount of history per frame, rather than starting every fragment in a single packet pump.
    if (!m_pendingUpdates.empty())
    {
        const auto update = m_pendingUpdates.front();
        m_pendingUpdates.pop_front();
        ApplyQuestUpdate(update);
        if (m_pendingUpdates.empty())
            AcknowledgeQuestUpdates();
    }

    // Checked every frame rather than on scene events: an action's done event fires before its fragment runs, and the
    // action only counts as complete afterwards, with no event of its own
    Vector<uint32_t> questFormIds;
    for (const auto& [questFormId, _] : m_heldStages)
        questFormIds.push_back(questFormId);

    for (uint32_t questFormId : questFormIds)
    {
        TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(questFormId));
        if (HeldStages* pHeld = GetHeldStages(questFormId, pQuest))
        {
            if (MayReleaseSceneStages(pQuest))
            {
                ReleaseStagesForWaitingPhases(pQuest, *pHeld);
                ReleaseUncheckedStages(pQuest, *pHeld);
            }
        }
    }

    Vector<uint32_t> stopped;
    for (const auto& [formId, stage] : m_pendingStops)
    {
        TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(formId));
        if (!pQuest || pQuest->IsStopped())
        {
            stopped.push_back(formId);
            continue;
        }

        if (pQuest->currentStage > stage)
        {
            spdlog::warn("Ignoring stale shared quest stop {:X} at stage {}; local stage is {}", formId, stage, pQuest->currentStage);
            stopped.push_back(formId);
            continue;
        }

        if (GetHeldStages(formId, pQuest) || !pQuest->IsStageReached(stage))
            continue;

        // Do not terminate an in-progress scene, even if the server has already seen a stop.
        bool playing = false;
        for (const auto* pScene : pQuest->scenes)
            playing |= pScene && pScene->isPlaying;
        if (playing)
            continue;

        ScopedQuestOverride override;
        StopQuest(formId);
        stopped.push_back(formId);
    }
    for (uint32_t formId : stopped)
        m_pendingStops.erase(formId);
}

void QuestService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_heldStages.clear();
    m_pendingUpdates.clear();
    m_receivedThrough = 0;
    m_pendingStops.clear();
    m_localScenes.clear();
    m_sharedScenes.clear();
}

BSTEventResult QuestService::OnEvent(const TESQuestStartStopEvent* apEvent, const EventDispatcher<TESQuestStartStopEvent>*)
{
    if (ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    // The quest only failed to start here (e.g. a required alias didn't fill), others may be running it
    if (apEvent->started && apEvent->failed)
        return BSTEventResult::kOk;

    if (!m_world.GetTransport().IsOnline())
        return BSTEventResult::kOk;

    spdlog::info("Quest start/stop event: {:X}", apEvent->formId);

    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
            return BSTEventResult::kOk;
     
        if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        {
            // Perhaps redundant, but necessary. We need the logging and
            // the lambda coming up is queued and runs later
            GameId Id;
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(pQuest->formID, Id))
            {
                spdlog::info(__FUNCTION__ ": queuing type none/misc quest gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                             Id.LogFormat(),  pQuest->currentStage, pQuest->IsStopped() ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started,
                             static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type), 
                             pQuest->formID, pQuest->fullName.value.AsAscii());
            }
        }
        
        m_world.GetRunner().Queue(
            [&, formId = pQuest->formID, stageId = pQuest->currentStage, stopped = !apEvent->started, type = pQuest->type]()
            {
                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (m_world.GetTransport().IsOnline() && modSys.GetServerModId(formId, Id))
                {
                    RequestQuestUpdate update;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = stopped ? RequestQuestUpdate::Stopped : RequestQuestUpdate::Started;
                    update.ClientQuestType = static_cast<std::underlying_type_t<TESQuest::Type>>(type); 

                    m_world.GetTransport().Send(update);
                }
            });
    }

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageEvent* apEvent, const EventDispatcher<TESQuestStageEvent>*)
{
    if (ScopedQuestOverride::IsOverriden())
        return BSTEventResult::kOk;

    if (!m_world.GetTransport().IsOnline())
        return BSTEventResult::kOk;

    spdlog::info("Quest stage event: {:X}, stage: {}", apEvent->formId, apEvent->stageId);

    // there is no reason to even fetch the quest object, since the event provides everything already....
    if (TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(apEvent->formId)))
    {
        if (IsNonSyncableQuest(pQuest))
            return BSTEventResult::kOk;

        if (pQuest->type == TESQuest::Type::None || pQuest->type == TESQuest::Type::Miscellaneous)
        {
            // Perhaps redundant, but necessary. We need the logging and
            // the lambda coming up is queued and runs later
            GameId Id;
            auto& modSys = m_world.GetModSystem();
            if (modSys.GetServerModId(pQuest->formID, Id))
            {
                spdlog::info(__FUNCTION__ ": queuing type none/misc quest gameId {:X} questStage {} questStatus {} questType {} formId {:X} name {}",
                             Id.LogFormat(), pQuest->currentStage,
                             RequestQuestUpdate::StageUpdate,
                             static_cast<std::underlying_type_t<TESQuest::Type>>(pQuest->type),
                             pQuest->formID, pQuest->fullName.value.AsAscii());
            }
        }

        m_world.GetRunner().Queue(
            [&, formId = apEvent->formId, stageId = apEvent->stageId, type = pQuest->type]()
            {
                GameId Id;
                auto& modSys = m_world.GetModSystem();
                if (m_world.GetTransport().IsOnline() && modSys.GetServerModId(formId, Id))
                {
                    RequestQuestUpdate update;
                    update.Id = Id;
                    update.Stage = stageId;
                    update.Status = RequestQuestUpdate::StageUpdate;
                    update.ClientQuestType = static_cast<std::underlying_type_t<TESQuest::Type>>(type);

                    m_world.GetTransport().Send(update);
                }
            });
    }

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESQuestStageItemDoneEvent* apEvent, const EventDispatcher<TESQuestStageItemDoneEvent>*)
{
    // Sent once the fragment of one of the stage's log entries has run, usually from a script thread
    m_world.GetRunner().Queue(
        [this, questFormId = apEvent->formId, stage = apEvent->stageId]()
        {
            auto it = m_heldStages.find(questFormId);
            if (it == m_heldStages.end())
                return;

            Vector<uint16_t>& pendingItems = it.value().PendingItems;
            auto itemIt = std::find(pendingItems.begin(), pendingItems.end(), stage);
            if (itemIt == pendingItems.end())
                return;

            pendingItems.erase(itemIt);
            spdlog::debug("Fragment of applied member quest stage {} for {:X} done, {} left", stage, questFormId, pendingItems.size());
        });

    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESSceneEvent* apEvent, const EventDispatcher<TESSceneEvent>*)
{
    ObserveScene(apEvent->sceneFormId);
    spdlog::debug("Scene {:X} {}", apEvent->sceneFormId, apEvent->type == 0 ? "begin" : "end");
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESSceneActionEvent* apEvent, const EventDispatcher<TESSceneActionEvent>*)
{
    ObserveScene(apEvent->sceneFormId);
    spdlog::debug("Scene {:X} action {} done, quest: {:X}, alias: {}", apEvent->sceneFormId, apEvent->actionIndex, apEvent->questFormId, apEvent->aliasId);
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESScenePhaseEvent* apEvent, const EventDispatcher<TESScenePhaseEvent>*)
{
    ObserveScene(apEvent->sceneFormId);
    if (!spdlog::should_log(spdlog::level::debug))
        return BSTEventResult::kOk;

    BGSScene* pScene = Cast<BGSScene>(TESForm::GetById(apEvent->sceneFormId));
    if (!pScene)
        return BSTEventResult::kOk;

    // A phase without completion conditions ends once every action running in it is complete
    String actions;
    for (BGSSceneAction* pAction : pScene->actions)
    {
        if (!pAction || !pAction->IsInPhase(apEvent->phaseIndex))
            continue;

        constexpr std::array cTypeNames{"dialogue", "package", "timer"};
        const uint32_t cType = static_cast<uint32_t>(pAction->GetType());
        actions += spdlog::fmt_lib::format(
            " [{} {} alias {} {}]", pAction->GetIndex(), cType < cTypeNames.size() ? cTypeNames[cType] : "unknown", pAction->actorId,
            pAction->IsComplete() ? "complete" : "running");
    }

    spdlog::debug("Scene {:X} phase {} {}, actions:{}", apEvent->sceneFormId, apEvent->phaseIndex, apEvent->type == 0 ? "begin" : "end", actions);
    return BSTEventResult::kOk;
}

BSTEventResult QuestService::OnEvent(const TESTopicInfoEvent* apEvent, const EventDispatcher<TESTopicInfoEvent>*)
{
    spdlog::debug(
        "Dialogue line {:X} {}, speaker: {:X}", apEvent->uiTopicInfoFormID, apEvent->eType == 0 ? "begin" : "end", apEvent->pSpeakerRef ? apEvent->pSpeakerRef->formID : 0);
    return BSTEventResult::kOk;
}

void QuestService::ObserveScene(uint32_t aFormId) noexcept
{
    if (!m_world.GetTransport().IsOnline() || !aFormId)
        return;

    // Event sinks can run on script threads. Only read engine scene state on
    // the main runner/update path after the event's own fragments have run.
    m_world.GetRunner().Queue([this, aFormId]()
    {
        if (m_world.GetTransport().IsOnline())
            m_localScenes.try_emplace(aFormId);
    });
}

std::vector<SceneReferenceAlias> QuestService::CaptureSceneAliases(BGSScene* apScene) const noexcept
{
    std::vector<SceneReferenceAlias> aliases;
    if (!apScene || !apScene->owningQuest)
        return aliases;

    auto& modSystem = m_world.GetModSystem();
    for (uint32_t aliasId : apScene->actorIds)
    {
        if (aliases.size() >= 255)
            break;
        if (std::any_of(aliases.begin(), aliases.end(), [aliasId](const auto& a) { return a.AliasId == aliasId; }))
            continue;

        // The existing getter takes a native ALID, not the index of a quest's
        // opaque alias array. Never guess the layout of that array.
        const auto* pReference = apScene->owningQuest->GetAliasedRef(aliasId);
        if (!pReference || pReference == PlayerCharacter::Get() ||
            pReference->formID == 0x14 || (pReference->formID >> 24) == 0xFF)
            continue;

        GameId reference{};
        if (!modSystem.GetServerModId(pReference->formID, reference) ||
            !reference.BaseId || modSystem.GetGameId(reference) != pReference->formID)
            continue;

        aliases.push_back({aliasId, reference});
    }
    std::sort(aliases.begin(), aliases.end(), [](const auto& a, const auto& b) { return a.AliasId < b.AliasId; });
    return aliases;
}

void QuestService::CheckSceneAliases(BGSScene* apScene, const std::vector<SceneReferenceAlias>& acLocal) noexcept
{
    const auto shared = m_sharedScenes.find(apScene->formID);
    const auto observer = m_localScenes.find(apScene->formID);
    if (observer == m_localScenes.end())
        return;

    auto& previous = observer->second.LastMismatches;
    if (shared == m_sharedScenes.end() || shared->second.Owner == m_world.GetTransport().GetLocalPlayerId())
    {
        previous.clear();
        return;
    }

    std::vector<SceneReferenceAlias> mismatches;
    for (const auto& expected : shared->second.State.ReferenceAliases)
    {
        const auto actual = std::find_if(acLocal.begin(), acLocal.end(),
            [&](const auto& a) { return a.AliasId == expected.AliasId; });
        if (actual == acLocal.end())
        {
            // A missing local sample may be an unloaded, player-specific or
            // temporary target. Do not declare it a conflicting assignment.
            continue;
        }
        if (actual->Reference != expected.Reference)
        {
            mismatches.push_back(expected);
            if (std::find(previous.begin(), previous.end(), expected) == previous.end())
                spdlog::warn("Quest alias mismatch: quest {:X}, scene {:X}, alias {}, canonical target {:X}, local target {:X}; native alias replacement not available",
                    apScene->owningQuest->formID, apScene->formID, expected.AliasId,
                    expected.Reference.LogFormat(), actual->Reference.LogFormat());
        }
    }
    previous = std::move(mismatches);
}

void QuestService::RefreshSceneStates() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_localScenes.begin(); it != m_localScenes.end();)
    {
        auto& local = it->second;
        auto* pScene = Cast<BGSScene>(TESForm::GetById(it->first));
        if (!pScene || !pScene->isPlaying)
        {
            if (local.Last.Active)
            {
                RequestSceneState ended{};
                ended.State = local.Last;
                ended.State.Active = false;
                if (!m_world.GetTransport().Send(ended))
                {
                    ++it;
                    continue;
                }
            }
            it = m_localScenes.erase(it);
            continue;
        }
        if (!pScene->owningQuest || IsNonSyncableQuest(pScene->owningQuest) ||
            pScene->owningQuest->type == TESQuest::Type::None ||
            pScene->owningQuest->type == TESQuest::Type::Miscellaneous)
        {
            it = m_localScenes.erase(it);
            continue;
        }

        RequestSceneState request{};
        if (!m_world.GetModSystem().GetServerModId(pScene->formID, request.State.Id) ||
            !m_world.GetModSystem().GetServerModId(pScene->owningQuest->formID, request.State.Quest))
        {
            ++it;
            continue;
        }

        request.State.Active = true;
        request.State.Phase = pScene->currentPhaseIndex;
        if (now >= local.NextAliasSample)
        {
            local.NextAliasSample = now + std::chrono::milliseconds(500);
            local.Aliases = CaptureSceneAliases(pScene);
            CheckSceneAliases(pScene, local.Aliases);
        }
        request.State.ReferenceAliases = local.Aliases;
        for (const auto* pAction : pScene->actions)
        {
            if (request.State.CompletedActions.size() == 255)
                break;
            if (pAction && pAction->IsComplete() && pAction->IsInPhase(request.State.Phase))
                request.State.CompletedActions.push_back(pAction->GetIndex());
        }
        if (request.State != local.Last || now >= local.NextHeartbeat)
        {
            if (m_world.GetTransport().Send(request))
            {
                local.Last = request.State;
                local.NextHeartbeat = now + std::chrono::seconds(2);
            }
        }
        ++it;
    }
}

void QuestService::OnSceneState(const NotifySceneState& acState) noexcept
{
    const uint32_t cFormId = m_world.GetModSystem().GetGameId(acState.State.Id);
    if (!cFormId)
        return;
    if (!acState.State.Active)
    {
        m_sharedScenes.erase(cFormId);
        return;
    }
    m_sharedScenes[cFormId] = acState;
    spdlog::debug("Shared scene {:X} owner {} generation {} phase {} completed actions {}",
        cFormId, acState.Owner, acState.Generation, acState.State.Phase, acState.State.CompletedActions.size());
}

bool QuestService::MayReleaseSceneStages(TESQuest* apQuest) const noexcept
{
    if (!apQuest)
        return false;

    // Do not push a follower past the owner by applying remote quest stages
    // when that follower's local scene is already further ahead. The owner
    // must never wait on itself. Skyrim still drives all native phase transitions.
    for (const auto* pScene : apQuest->scenes)
    {
        if (!pScene || !pScene->isPlaying || pScene->currentPhaseIndex == UINT32_MAX)
            continue;
        const auto it = m_sharedScenes.find(pScene->formID);
        if (it == m_sharedScenes.end() || it->second.Owner == m_world.GetTransport().GetLocalPlayerId())
            continue;
        const auto& state = it->second.State;
        if (state.Phase == UINT32_MAX || pScene->currentPhaseIndex > state.Phase)
            return false;
    }
    return true;
}

static bool Compare(TESConditionItem::OpCode aOpCode, float aLhs, float aRhs) noexcept
{
    switch (aOpCode)
    {
    case TESConditionItem::OpCode::kEqualTo: return aLhs == aRhs;
    case TESConditionItem::OpCode::kNotEqualTo: return aLhs != aRhs;
    case TESConditionItem::OpCode::kGreaterThan: return aLhs > aRhs;
    case TESConditionItem::OpCode::kGreaterThanOrEqualTo: return aLhs >= aRhs;
    case TESConditionItem::OpCode::kLessThan: return aLhs < aRhs;
    case TESConditionItem::OpCode::kLessThanOrEqualTo: return aLhs <= aRhs;
    default: return false;
    }
}

// Whether setting the stage turns a GetStage/GetStageDone check on the quest from false to true. With aOrChecksLater,
// also whether it changes the check's result at all, or the check is on the same or a later stage.
static bool IsCheckedByStage(const TESCondition& acConditions, TESQuest* apQuest, uint16_t aStage, bool aOrChecksLater) noexcept
{
    for (const TESConditionItem* pItem = acConditions.pHead; pItem; pItem = pItem->pNext)
    {
        if (pItem->UsesGlobal() || pItem->params[0] != apQuest)
            continue;

        const TESConditionItem::OpCode cOpCode = pItem->GetOpCode();
        float checkedStage = 0.f;
        bool before = false;
        bool after = false;
        if (pItem->function == TESConditionItem::kGetStage)
        {
            // Stages only go up, GetStage returns the highest one set
            checkedStage = pItem->comparisonValue;
            before = Compare(cOpCode, apQuest->currentStage, pItem->comparisonValue);
            after = Compare(cOpCode, std::max(apQuest->currentStage, aStage), pItem->comparisonValue);
        }
        else if (pItem->function == TESConditionItem::kGetStageDone)
        {
            const uint16_t cStage = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(pItem->params[1]));
            checkedStage = cStage;
            if (cStage == aStage)
            {
                before = Compare(cOpCode, 0.f, pItem->comparisonValue);
                after = Compare(cOpCode, 1.f, pItem->comparisonValue);
            }
        }
        else
        {
            continue;
        }

        if (!before && after)
            return true;

        if (aOrChecksLater && (before != after || checkedStage >= aStage))
            return true;
    }

    return false;
}

// Whether the phase's own lines are done, so passing its completion conditions won't cut one off. Timers and packages
// don't hold such a phase back, and packages never complete on clients that don't own the NPC.
static bool IsPhaseWaitingOnConditions(BGSScene* apScene, uint32_t aPhaseIndex) noexcept
{
    for (BGSSceneAction* pAction : apScene->actions)
    {
        if (!pAction || !pAction->IsInPhase(aPhaseIndex))
            continue;

        if (pAction->GetType() != BGSSceneAction::Type::kDialogue || pAction->IsComplete() || pAction->Loops())
            continue;

        // The player's own lines wait for this player to answer, while a member may have answered instead
        if (apScene->owningQuest && apScene->owningQuest->GetAliasedRef(pAction->actorId) == PlayerCharacter::Get())
            continue;

        return false;
    }

    return true;
}

// Whether a phase the playing scenes have yet to finish checks the stage or a later one, or checks differently with it
// (a phase waiting on GetStage == N). Scenes that only end or repeat on it don't count (MQ105's sprint trial).
static bool IsCheckedByPlayingScene(TESQuest* apQuest, uint16_t aStage) noexcept
{
    for (BGSScene* pScene : apQuest->scenes)
    {
        if (!pScene)
            continue;

        for (uint32_t i = pScene->GetNextPhaseIndex(); i < pScene->phases.length; ++i)
        {
            const BGSScenePhase* pPhase = pScene->phases[i];
            if (!pPhase)
                continue;

            // The running phase is past its start conditions
            const bool cIsRunning = pScene->currentPhaseIndex == i;
            if ((!cIsRunning && IsCheckedByStage(pPhase->startConditions, apQuest, aStage, true)) ||
                IsCheckedByStage(pPhase->completionConditions, apQuest, aStage, true))
            {
                return true;
            }
        }
    }

    return false;
}

void QuestService::HoldStage(TESQuest* apQuest, uint16_t aStage) noexcept
{
    Vector<uint16_t>& stages = m_heldStages[apQuest->formID].Stages;
    if (std::find(stages.begin(), stages.end(), aStage) == stages.end())
        stages.push_back(aStage);

    spdlog::info("Holding member quest stage {} for {:X} until no playing scene of it checks it", aStage, apQuest->formID);
}

static void ApplyHeldStages(TESQuest* apQuest, HeldStages& aHeld, size_t aCount) noexcept
{
    ScopedQuestOverride _;

    // In order, a later stage's scripts can rely on the earlier ones
    for (size_t i = 0; i < aCount; ++i)
    {
        // A scene the fragments start only plays once they've run, and the next stage may be what it waits on. Each log
        // entry reports when its fragment is done.
        const uint16_t cStage = aHeld.Stages[i];
        if (apQuest->ScriptSetStage(cStage))
            aHeld.PendingItems.insert(aHeld.PendingItems.end(), apQuest->GetStageItemCount(cStage), cStage);
        else if (!apQuest->IsStageReached(cStage))
            spdlog::warn("Unable to apply held shared quest {:X} stage {}; reconnect to retry journal", apQuest->formID, cStage);
    }

    aHeld.Stages.erase(aHeld.Stages.begin(), aHeld.Stages.begin() + aCount);
}

static void ReleaseStagesForWaitingPhases(TESQuest* apQuest, HeldStages& aHeld) noexcept
{
    // A phase whose own actions are done and whose completion conditions want a held stage only moves on with it.
    // Release up to the first stage that does it, the leader's scene then carries on at its own pace.
    for (BGSScene* pScene : apQuest->scenes)
    {
        if (aHeld.Stages.empty())
            break;

        if (!pScene || !pScene->isPlaying || pScene->currentPhaseIndex >= pScene->phases.length)
            continue;

        BGSScenePhase* pPhase = pScene->phases[pScene->currentPhaseIndex];
        if (!pPhase)
            continue;

        // Conditions first, they rarely mention a held stage and are cheaper than walking the phase's actions
        const auto cStageIt = std::find_if(
            aHeld.Stages.begin(), aHeld.Stages.end(),
            [&](uint16_t aStage) { return IsCheckedByStage(pPhase->completionConditions, apQuest, aStage, false); });
        if (cStageIt == aHeld.Stages.end() || !IsPhaseWaitingOnConditions(pScene, pScene->currentPhaseIndex))
            continue;

        spdlog::info(
            "Releasing held member quest stages for {:X} up to {}, scene {:X} phase {} waits on it", apQuest->formID, *cStageIt, pScene->formID,
            pScene->currentPhaseIndex);
        ApplyHeldStages(apQuest, aHeld, cStageIt - aHeld.Stages.begin() + 1);
    }
}

// Applies held stages in order while no playing scene checks the next one, e.g. once the scenes that did have ended.
// Waits for the fragments of the stages it applied first.
static void ReleaseUncheckedStages(TESQuest* apQuest, HeldStages& aHeld) noexcept
{
    while (!aHeld.Stages.empty() && aHeld.PendingItems.empty() && !IsCheckedByPlayingScene(apQuest, aHeld.Stages.front()))
    {
        spdlog::info("Applying held member quest stage {} for {:X}, no playing scene checks it", aHeld.Stages.front(), apQuest->formID);
        ApplyHeldStages(apQuest, aHeld, 1);
    }
}

HeldStages* QuestService::GetHeldStages(uint32_t aQuestFormId, TESQuest* apQuest) noexcept
{
    auto it = m_heldStages.find(aQuestFormId);
    if (it == m_heldStages.end())
        return nullptr;

    // Setting a stage on a stopped quest starts it again, and scenes also end because their quest stopped
    HeldStages& held = it.value();
    if (!apQuest || apQuest->IsStopped())
    {
        if (!held.Stages.empty())
            spdlog::info("Dropping held member quest stages for {:X}, quest stopped or missing", aQuestFormId);

        m_heldStages.erase(it);
        return nullptr;
    }

    // Reached by the leader's own scripts in the meantime
    std::erase_if(
        held.Stages,
        [apQuest](uint16_t aStage)
        {
            if (!apQuest->IsStageReached(aStage))
                return false;

            spdlog::info("Dropping held member quest stage {} for {:X}, already reached here", aStage, apQuest->formID);
            return true;
        });

    if (held.Stages.empty() && held.PendingItems.empty())
    {
        m_heldStages.erase(it);
        return nullptr;
    }

    return &held;
}

void QuestService::OnQuestUpdate(const NotifyQuestUpdate& acBatch) noexcept
{
    if (!m_world.GetTransport().IsOnline() || acBatch.Updates.empty() || !acBatch.Through)
        return;
    if (acBatch.Through == m_receivedThrough && m_pendingUpdates.empty())
    {
        AcknowledgeQuestUpdates();
        return;
    }
    if (acBatch.Through <= m_receivedThrough || !m_pendingUpdates.empty())
        return;

    m_receivedThrough = acBatch.Through;
    m_pendingUpdates.assign(acBatch.Updates.begin(), acBatch.Updates.end());
}

void QuestService::AcknowledgeQuestUpdates() noexcept
{
    // This acknowledges delivery to the local quest service. A reconnect always
    // replays from the start because the character may have loaded an older save.
    RequestQuestSyncAck ack{};
    ack.Through = m_receivedThrough;
    m_world.GetTransport().Send(ack);
}

void QuestService::ApplyQuestUpdate(const SharedQuestUpdate& acUpdate) noexcept
{
    const uint32_t cFormId = m_world.GetModSystem().GetGameId(acUpdate.Id);
    if (!cFormId)
        return;

    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(cFormId));
    if (!pQuest || IsNonSyncableQuest(pQuest))
        return;

    if (acUpdate.Status == SharedQuestUpdate::Stopped)
    {
        if (!pQuest->IsStopped())
            m_pendingStops[cFormId] = acUpdate.Stage;
        return;
    }

    if (acUpdate.Status != SharedQuestUpdate::Started && acUpdate.Status != SharedQuestUpdate::StageUpdate)
        return;

    // Never restart a locally completed quest just to replay an old stage.
    if (pQuest->IsStopped() && (pQuest->flags & TESQuest::Flags::Completed))
        return;

    if (pQuest->IsStageReached(acUpdate.Stage))
    {
        // A fresh quest can have currentStage == 0 before it is actually running.
        // Use the already-exposed native start routine, without forcing alias filling.
        if (acUpdate.Status == SharedQuestUpdate::Started && acUpdate.Stage == 0 &&
            pQuest->IsStopped() && !pQuest->alreadyRun && !pQuest->IsStageDone(0))
        {
            ScopedQuestOverride override;
            bool started = false;
            pQuest->EnsureQuestStarted(started, false);
            if (!pQuest->IsStopped())
                pQuest->SetActive(true);
            else
                spdlog::warn("Unable to start shared quest {:X} at stage zero; reconnect to retry journal", cFormId);
        }
        return;
    }

    if (!pQuest->IsStopped())
    {
        // All clients protect their scenes, not just a party leader. Hold subsequent stages
        // until earlier fragments finish or a scene is ready for the required stage.
        HoldStage(pQuest, acUpdate.Stage);
        return;
    }

    // Use the existing native stage path to start a stopped quest. Track its fragments just
    // as for an already-running quest. No guessed engine offsets or new hooks are needed.
    ScopedQuestOverride override;
    if (pQuest->ScriptSetStage(acUpdate.Stage))
    {
        if (acUpdate.Status == SharedQuestUpdate::Started)
            pQuest->SetActive(true);
        auto& held = m_heldStages[cFormId];
        held.PendingItems.insert(held.PendingItems.end(), pQuest->GetStageItemCount(acUpdate.Stage), acUpdate.Stage);
    }
    else
    {
        spdlog::warn("Unable to apply shared quest {:X} stage {}; reconnect to retry journal", cFormId, acUpdate.Stage);
    }
}

bool QuestService::StopQuest(uint32_t aformId)
{
    TESQuest* pQuest = Cast<TESQuest>(TESForm::GetById(aformId));
    if (pQuest)
    {
        pQuest->SetActive(false);
        pQuest->SetStopped();
        return true;
    }

    return false;
}

static constexpr std::array kNonSyncableQuestIds = std::to_array<uint32_t>({
    0x2BA16,   // Werewolf transformation quest
    0x20071D0, // Vampire transformation quest
    0x3AC44,   // MS13BleakFallsBarrowLeverScene
    // 0xFE014801,  // Unknown dynamic ID, kept as note, maybe lookup correct ID this game?
    0xF2593 // Skill experience quest
});

bool QuestService::IsNonSyncableQuest(TESQuest* apQuest)
{
    // Quests with no quest stages are never synced. Most TESQues::Type:: quests should
    // be synced, including Type::None and Type::Miscellaneous, but there are a few
    // known exceptions that should be excluded that are in the table.
    const char* pName = apQuest->idName.AsAscii();
    const bool cPersonal = pName && (std::strncmp(pName, "RelationshipMarriage", 20) == 0 ||
        std::strcmp(pName, "PlayerWerewolfQuest") == 0 || std::strcmp(pName, "PlayerVampireQuest") == 0);
    return apQuest->stages.Empty() || (apQuest->formID >> 24) == 0xFF ||
           (apQuest->flags & TESQuest::Flags::AllowRepeatStages) || cPersonal ||
           std::find(kNonSyncableQuestIds.begin(), kNonSyncableQuestIds.end(), apQuest->formID) != kNonSyncableQuestIds.end();
}

void QuestService::DebugDumpQuests()
{
    auto& quests = ModManager::Get()->quests;
    for (TESQuest* pQuest : quests)
        spdlog::info("{:X}|{}|{}|{}", pQuest->formID, (uint8_t)pQuest->type, pQuest->priority, pQuest->idName.AsAscii());
}
