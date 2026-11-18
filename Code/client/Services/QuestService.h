#pragma once

#include <World.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/NotifyQuestUpdate.h>
#include <deque>
#include <map>
#include <chrono>
#include <Messages/NotifySceneState.h>

struct DisconnectedEvent;
struct UpdateEvent;

struct TESQuest;
struct BGSScene;

/** @brief Applies server-wide quest progression using the native client quest engine. */
class QuestService final : public BSTEventSink<TESQuestStartStopEvent>,
                           BSTEventSink<TESQuestStageEvent>,
                           BSTEventSink<TESQuestStageItemDoneEvent>,
                           BSTEventSink<TESSceneEvent>,
                           BSTEventSink<TESSceneActionEvent>,
                           BSTEventSink<TESScenePhaseEvent>,
                           BSTEventSink<TESTopicInfoEvent>
{
public:
    QuestService(World&, entt::dispatcher&);
    ~QuestService() = default;

    static bool IsNonSyncableQuest(TESQuest* apQuest);
    static void DebugDumpQuests();
    static bool StopQuest(uint32_t aformId);

    struct HeldStages
    {
        // Stages not applied yet, in server journal order
        Vector<uint16_t> Stages{};
        // A stage per log entry of the applied stages whose fragment hasn't run yet
        Vector<uint16_t> PendingItems{};
    };

private:
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;

    BSTEventResult OnEvent(const TESQuestStartStopEvent*, const EventDispatcher<TESQuestStartStopEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageEvent*, const EventDispatcher<TESQuestStageEvent>*) override;
    BSTEventResult OnEvent(const TESQuestStageItemDoneEvent*, const EventDispatcher<TESQuestStageItemDoneEvent>*) override;
    BSTEventResult OnEvent(const TESSceneEvent*, const EventDispatcher<TESSceneEvent>*) override;
    BSTEventResult OnEvent(const TESSceneActionEvent*, const EventDispatcher<TESSceneActionEvent>*) override;
    BSTEventResult OnEvent(const TESScenePhaseEvent*, const EventDispatcher<TESScenePhaseEvent>*) override;
    BSTEventResult OnEvent(const TESTopicInfoEvent*, const EventDispatcher<TESTopicInfoEvent>*) override;

    void OnQuestUpdate(const NotifyQuestUpdate&) noexcept;
    void HoldStage(TESQuest* apQuest, uint16_t aStage) noexcept;
    // The quest's held stages, while it has stages to apply or fragments to wait for. Drops the stages that can't be
    // applied anymore.
    HeldStages* GetHeldStages(uint32_t aQuestFormId, TESQuest* apQuest) noexcept;
    void ApplyQuestUpdate(const NotifyQuestUpdate&) noexcept;
    void ObserveScene(uint32_t aFormId) noexcept;
    void RefreshSceneStates() noexcept;
    void OnSceneState(const NotifySceneState& acState) noexcept;
    bool MayReleaseSceneStages(TESQuest* apQuest) const noexcept;
    std::vector<SceneReferenceAlias> CaptureSceneAliases(BGSScene* apScene) const noexcept;
    void CheckSceneAliases(BGSScene* apScene, const std::vector<SceneReferenceAlias>& acLocal) noexcept;

    World& m_world;
    // Keyed by quest form ID
    Map<uint32_t, HeldStages> m_heldStages{};

    struct LocalScene
    {
        SceneState Last{};
        std::chrono::steady_clock::time_point NextHeartbeat{};
        std::chrono::steady_clock::time_point NextAliasSample{};
        std::vector<SceneReferenceAlias> Aliases;
        std::vector<SceneReferenceAlias> LastMismatches;
    };
    std::map<uint32_t, LocalScene> m_localScenes;
    std::map<uint32_t, NotifySceneState> m_sharedScenes;
    entt::scoped_connection m_sceneConnection;

    entt::scoped_connection m_joinedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_disconnectedConnection;
    std::deque<NotifyQuestUpdate> m_pendingUpdates;
    Map<uint32_t, uint16_t> m_pendingStops;
    entt::scoped_connection m_questUpdateConnection;
};
