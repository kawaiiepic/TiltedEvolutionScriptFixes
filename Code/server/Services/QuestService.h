#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

#include <map>
#include <chrono>
#include <Campaign/QuestPolicies.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifySceneState.h>

struct World;
struct Player;
struct PlayerJoinEvent;
struct RequestQuestUpdate;
struct RequestQuestSyncAck;
struct RequestSceneState;
struct PlayerLeaveEvent;
struct UpdateEvent;

// The server owns an ordered campaign journal. The game clients still run native quest fragments.
class QuestService
{
public:
    QuestService(World& aWorld, entt::dispatcher& aDispatcher);
    void ShowCampaignStatus() const noexcept;
    void BackupCampaign() const noexcept;

private:
    struct QuestSync
    {
        size_t Cursor{};
        NotifyQuestUpdate Pending;
        std::chrono::steady_clock::time_point RetryAt{};
    };

    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnQuestSyncTick(const UpdateEvent&) noexcept;
    void OnQuestSyncAck(const PacketEvent<RequestQuestSyncAck>& acEvent) noexcept;
    bool ResolveQuestKey(const GameId& acId, const Player& acPlayer, Campaign::QuestKey& aKey) const noexcept;
    void QueueEntry(NotifyQuestUpdate& aBatch, const Campaign::JournalRecord& acRecord, const Player& acPlayer) const noexcept;

    struct SceneObserver
    {
        SceneState State{};
        std::chrono::steady_clock::time_point LastSeen{};
    };

    struct SharedScene
    {
        NotifySceneState Canonical{};
        std::map<uint32_t, SceneObserver> Observers;
        bool Ended{};
    };

    void OnSceneState(const PacketEvent<RequestSceneState>& acEvent) noexcept;
    void OnSceneTick(const UpdateEvent&) noexcept;
    void RemoveSceneObserver(SharedScene& aScene, uint32_t aPlayerId, bool aFinished) noexcept;
    void BroadcastScene(const NotifySceneState& acState) noexcept;
    static void SendScene(const NotifySceneState& acState, const Player& acPlayer) noexcept;

    std::map<uint64_t, SharedScene> m_scenes;
    uint32_t m_nextSceneGeneration{};
    std::chrono::steady_clock::time_point m_nextSceneTick{};
    entt::scoped_connection m_sceneConnection;
    entt::scoped_connection m_sceneTickConnection;
    entt::scoped_connection m_leaveConnection;

    World& m_world;
    Campaign::QuestJournal m_journal;
    Campaign::QuestPolicies m_policies;
    std::map<uint32_t, QuestSync> m_questSync;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_joinConnection;
    entt::scoped_connection m_questSyncTickConnection;
    entt::scoped_connection m_questSyncAckConnection;
};
