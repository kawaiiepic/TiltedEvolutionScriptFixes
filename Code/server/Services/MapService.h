#pragma once

#include <Events/PacketEvent.h>
#include <Campaign/DiscoveryJournal.h>
#include <Campaign/FormSyncPolicies.h>

struct World;
struct RequestSetWaypoint;
struct RequestRemoveWaypoint;
struct RequestMapDiscovery;
struct PlayerJoinEvent;

/**
 * @brief Handles player specific actions that might change the information needed by other clients about that player.
 */
struct MapService
{
    MapService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~MapService() noexcept = default;

    TP_NOCOPYMOVE(MapService);
    static bool SharesDiscoveries() noexcept;

  protected:
    void OnSetWaypointRequest(const PacketEvent<RequestSetWaypoint>& acMessage) const noexcept;
    void OnRemoveWaypointRequest(const PacketEvent<RequestRemoveWaypoint>& acMessage) const noexcept;

  private:
    void OnMapDiscovery(const PacketEvent<RequestMapDiscovery>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;

    World& m_world;
    Campaign::FormSyncPolicies m_discoveryPolicies{"shared-discovery-policies.conf"};
    Campaign::DiscoveryJournal m_discoveries{"shared-discoveries.log"};
    entt::scoped_connection m_discoveryConnection;
    entt::scoped_connection m_joinConnection;

    entt::scoped_connection m_playerSetWaypointConnection;
    entt::scoped_connection m_playerRemoveWaypointConnection;
};
