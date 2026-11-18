#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Messages/NotifyTeleport.h>
#include <optional>

struct World;
struct TransportService;

struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct ServerSettings;
struct GridCellChangeEvent;
struct CellChangeEvent;
struct PlayerLevelEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;
struct ActivateEvent;

struct NotifyPlayerRespawn;

/**
 * @brief Handles logic related to the local player.
 */
struct PlayerService : BSTEventSink<TESFastTravelEndEvent>
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnServerSettingsReceived(const ServerSettings& acSettings) noexcept;
    void OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) const noexcept;
    void OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept;
    void OnCellChangeEvent(const CellChangeEvent& acEvent) noexcept;
    void OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;
    void OnActivate(const ActivateEvent& acEvent) noexcept;
    void OnNotifyTeleport(const NotifyTeleport& acMessage) noexcept;
    BSTEventResult OnEvent(const TESFastTravelEndEvent*, const EventDispatcher<TESFastTravelEndEvent>*) override;

private:
    /**
     * @brief Run the respawn timer, and if it hits 0, respawn the player.
     */
    void RunRespawnUpdates() noexcept;
    void RunPostDeathUpdates() noexcept;
    /**
     * @brief Make sure difficulty doesn't get changed while connected
     */
    void RunDifficultyUpdates() const noexcept;
    void RunLevelUpdates() const noexcept;
    void RunBeastFormDetection() const noexcept;
    void RunCarriageTravelUpdates() noexcept;
    void ApplyTeleport(const NotifyTeleport& acMessage) noexcept;

    void ToggleDeathSystem(bool aSet) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    struct CarriageBoarding
    {
        GameId Seat;
        GameId Origin;
        GameId WorldSpace;
        glm::vec3 Position;
        std::chrono::steady_clock::time_point ExpiresAt;
        uint32_t Token;
        bool Arrived{};
        bool SawRidingControls{};
        bool SawLoading{};
    };
    std::optional<CarriageBoarding> m_carriage;
    std::optional<NotifyTeleport> m_carriageTeleport;
    GameId m_reportedCell{};
    uint32_t m_nextCarriageToken{};

    std::chrono::steady_clock::time_point m_respawnDeadline;
    int32_t m_serverDifficulty = 6;
    int32_t m_previousDifficulty = 6;

    bool m_isDeathSystemEnabled = true;

    bool m_knockdownStart = false;
    std::chrono::steady_clock::time_point m_knockdownDeadline;

    bool m_godmodeStart = false;
    std::chrono::steady_clock::time_point m_godmodeDeadline;

    uint32_t m_cachedMainSpellId = 0;
    uint32_t m_cachedSecondarySpellId = 0;
    uint32_t m_cachedPowerId = 0;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_settingsConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_gridCellChangeConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
    entt::scoped_connection m_carriageActivateConnection;
    entt::scoped_connection m_teleportConnection;
};
