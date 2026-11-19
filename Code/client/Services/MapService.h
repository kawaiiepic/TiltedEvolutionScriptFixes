#pragma once

#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <Structs/GameId.h>
#include <chrono>
#include <unordered_set>

struct World;
struct TransportService;

struct SetWaypointEvent;
struct RemoveWaypointEvent;
struct NotifySetWaypoint;
struct NotifyRemoveWaypoint;
struct NotifyMapDiscoveries;
struct DisconnectedEvent;
struct UpdateEvent;
struct ServerSettings;
struct MapMarkerData;

struct LocationDiscoveryEvent
{
    MapMarkerData* pMarker;
    const char* pWorldspaceId;
};
static_assert(sizeof(LocationDiscoveryEvent) == 0x10);

/**
 * @brief Handles logic related to the local player.
 */
struct MapService final : BSTEventSink<LocationDiscoveryEvent>
{
    MapService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~MapService() noexcept;

    TP_NOCOPYMOVE(MapService);

  protected:
    void OnSetWaypoint(const SetWaypointEvent& acMessage) noexcept;
    void OnRemoveWaypoint(const RemoveWaypointEvent& acMessage) noexcept;
    void OnNotifySetWaypoint(const NotifySetWaypoint& acMessage) noexcept;
    void OnNotifyRemoveWaypoint(const NotifyRemoveWaypoint& acMessage) noexcept;

  private:
    BSTEventResult OnEvent(const LocationDiscoveryEvent* apEvent, const EventDispatcher<LocationDiscoveryEvent>*) override;
    void ReportDiscovery(uintptr_t aMarkerAddress) noexcept;
    void OnMapDiscoveries(const NotifyMapDiscoveries& acMessage) noexcept;
    void ApplyMapDiscoveries(const UpdateEvent&) noexcept;
    void OnDiscoverySettings(const ServerSettings& acSettings) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    EventDispatcher<LocationDiscoveryEvent>* m_pDiscoverySource{};
    std::unordered_set<GameId> m_pendingDiscoveries;
    std::chrono::steady_clock::time_point m_nextDiscoveryUpdate{};
    entt::scoped_connection m_discoveryConnection;
    entt::scoped_connection m_discoverySettingsConnection;
    entt::scoped_connection m_discoveryUpdateConnection;
    entt::scoped_connection m_discoveryDisconnectConnection;
    entt::scoped_connection m_playerSetWaypointConnection;
    entt::scoped_connection m_playerRemoveWaypointConnection;
    entt::scoped_connection m_playerNotifySetWaypointConnection;
    entt::scoped_connection m_playerNotifyRemoveWaypointConnection;
};
