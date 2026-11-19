#include <Services/MapService.h>
#include <GameServer.h>
#include <Components.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/NotifyPlayerMapMarkers.h>
#include <cmath>
#include <map>
#include <Messages/MapDiscovery.h>
#include <Setting.h>

#include <Messages/NotifyRemoveWaypoint.h>
#include <Messages/NotifySetWaypoint.h>
#include <Messages/RequestRemoveWaypoint.h>
#include <Messages/RequestSetWaypoint.h>

namespace
{
Console::Setting bSharedMapDiscoveries{"Gameplay:bSharedMapDiscoveries", "Share and persist discovered map locations", false};
}

bool MapService::SharesDiscoveries() noexcept
{
    return bSharedMapDiscoveries;
}

MapService::MapService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_discoveryConnection = aDispatcher.sink<PacketEvent<RequestMapDiscovery>>().connect<&MapService::OnMapDiscovery>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&MapService::PublishPlayerMarkers>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&MapService::OnPlayerJoin>(this);
    if (!m_discoveryPolicies.IsHealthy())
        spdlog::error("Unable to read shared-discovery-policies.conf (error line {}); shared map discoveries are disabled.",
            m_discoveryPolicies.ErrorLine());
    if (!m_discoveries.IsHealthy())
        spdlog::error("Unable to read shared-discoveries.log; shared map discoveries are disabled.");
      m_playerSetWaypointConnection = 
          aDispatcher.sink<PacketEvent<RequestSetWaypoint>>().connect<&MapService::OnSetWaypointRequest>(this);
      m_playerRemoveWaypointConnection =
          aDispatcher.sink<PacketEvent<RequestRemoveWaypoint>>().connect<&MapService::OnRemoveWaypointRequest>(this);
}

void MapService::PublishPlayerMarkers(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextMarkerUpdate)
        return;
    m_nextMarkerUpdate = now + std::chrono::milliseconds(250);

    std::map<uint32_t, NotifyPlayerMapMarkers> snapshots;
    ++m_markerTick;
    for (const auto* pPlayer : m_world.GetPlayerManager())
    {
        const auto& party = pPlayer->GetParty().JoinedPartyId;
        const auto& cell = pPlayer->GetCellComponent();
        if (!party || !cell.Cell)
            continue;

        auto& snapshot = snapshots[*party];
        snapshot.Tick = m_markerTick;
        const auto character = pPlayer->GetCharacter();
        const auto* pMovement = character ? m_world.try_get<MovementComponent>(*character) : nullptr;
        const auto* pCharacterCell = character ? m_world.try_get<CellIdComponent>(*character) : nullptr;
        if (!pMovement || !pCharacterCell || pCharacterCell->WorldSpaceId != cell.WorldSpaceId ||
            (!cell.WorldSpaceId && pCharacterCell->Cell != cell.Cell) ||
            snapshot.Players.size() >= NotifyPlayerMapMarkers::MaxPlayers ||
            !std::isfinite(pMovement->Position.x) || !std::isfinite(pMovement->Position.y) || !std::isfinite(pMovement->Position.z))
            continue;

        auto& marker = snapshot.Players.emplace_back();
        marker.Id = pPlayer->GetId();
        marker.Name = pPlayer->GetUsername();
        marker.Cell = cell.Cell;
        marker.WorldSpace = cell.WorldSpaceId;
        marker.Entrance = pPlayer->InteriorEntrance;
        marker.EntranceWorldSpace = pPlayer->EntranceWorldSpace;
        marker.EntrancePosition = pPlayer->EntrancePosition;
        marker.Position = pMovement->Position;
    }

    for (const auto* pPlayer : m_world.GetPlayerManager())
    {
        const auto& party = pPlayer->GetParty().JoinedPartyId;
        if (!party || !pPlayer->GetCellComponent().Cell)
            continue;
        if (const auto it = snapshots.find(*party); it != snapshots.end())
            pPlayer->Send(it->second, TiltedPhoques::kUnreliableNoNagle);
    }
}

void MapService::OnSetWaypointRequest(const PacketEvent<RequestSetWaypoint>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;

    NotifySetWaypoint notify{};
    notify.Position = message.Position;
    notify.WorldSpaceFormID = message.WorldSpaceFormID;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (!partyComponent.JoinedPartyId.has_value())
        return;

    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}

void MapService::OnRemoveWaypointRequest(const PacketEvent<RequestRemoveWaypoint>& acMessage) const noexcept
{
    NotifyRemoveWaypoint notify{};

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (!partyComponent.JoinedPartyId.has_value())
        return;

    GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
}


void MapService::OnMapDiscovery(const PacketEvent<RequestMapDiscovery>& acMessage) noexcept
{
    if (!bSharedMapDiscoveries || !acMessage.pPlayer->GetCharacter())
        return;
    const auto marker = acMessage.Packet.Marker;
    const auto key = m_world.ctx().at<ModsComponent>().FindFormKey(marker, *acMessage.pPlayer);
    if (!key || !m_discoveryPolicies.IsShared(*key, true) ||
        m_discoveries.Discover(*key) != Campaign::DiscoveryJournal::Result::Added)
        return;

    NotifyMapDiscoveries notify{};
    notify.Markers.push_back(marker);
    for (const Player* pPlayer : m_world.GetPlayerManager())
        if (pPlayer != acMessage.pPlayer && pPlayer->HasModId(marker.ModId))
            pPlayer->Send(notify);
}

void MapService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    if (!bSharedMapDiscoveries || !m_discoveries.IsHealthy())
        return;
    const auto& mods = m_world.ctx().at<ModsComponent>();
    NotifyMapDiscoveries notify{};
    for (const auto& key : m_discoveries.GetDiscoveries())
    {
        if (!m_discoveryPolicies.IsShared(key, true))
            continue;
        const auto marker = mods.FindFormId(key);
        if (!marker || !acEvent.pPlayer->HasModId(marker->ModId))
            continue;
        notify.Markers.push_back(*marker);
        if (notify.Markers.size() == NotifyMapDiscoveries::MaxMarkers)
        {
            acEvent.pPlayer->Send(notify);
            notify.Markers.clear();
        }
    }
    if (!notify.Markers.empty())
        acEvent.pPlayer->Send(notify);
}
