#include <Services/MapService.h>

#include <World.h>
#include <Games/TES.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/MapDiscovery.h>
#include <ExtraData/ExtraMapMarker.h>
#include <Interface/UI.h>
#include <Structs/GridCellCoords.h>
#include <Structs/ServerSettings.h>
#include <VersionDb.h>

#include <Events/SetWaypointEvent.h>
#include <Events/RemoveWaypointEvent.h>
#include <Messages/NotifySetWaypoint.h>
#include <Messages/NotifyRemoveWaypoint.h>
#include <Messages/RequestSetWaypoint.h>
#include <Messages/RequestRemoveWaypoint.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

namespace
{
MapMarkerData* GetShareableMapMarker(const TESObjectREFR& acReference) noexcept
{
    if (acReference.IsTemporary() || acReference.IsDeleted() || acReference.IsDisabled())
        return nullptr;
    auto* pExtra = static_cast<ExtraMapMarker*>(acReference.extraData.GetByType(ExtraDataType::MapMarker));
    auto* pMarker = pExtra ? pExtra->mapData : nullptr;
    if (!pMarker || !pMarker->type || pMarker->type > 59 || (pMarker->flags & MapMarkerData::ShowAllHidden))
        return nullptr;
    return pMarker;
}

const TESObjectREFR* FindDiscoveredMarker(TESObjectCELL* apCell, uintptr_t aMarkerAddress) noexcept
{
    if (!apCell || !apCell->refData.refArray)
        return nullptr;
    for (uint32_t i = 0; i < apCell->refData.capacity; ++i)
    {
        const auto* pRef = apCell->refData.refArray[i].Get();
        if (!pRef)
            continue;
        const auto* pMarker = GetShareableMapMarker(*pRef);
        if (pMarker && reinterpret_cast<uintptr_t>(pMarker) == aMarkerAddress &&
            (pMarker->flags & (MapMarkerData::Visible | MapMarkerData::CanTravelTo)) ==
                (MapMarkerData::Visible | MapMarkerData::CanTravelTo))
            return pRef;
    }
    return nullptr;
}
}

MapService::MapService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld), m_dispatcher(aDispatcher), m_transport(aTransport)
{
    m_discoveryConnection = aDispatcher.sink<NotifyMapDiscoveries>().connect<&MapService::OnMapDiscoveries>(this);
    m_discoveryUpdateConnection = aDispatcher.sink<UpdateEvent>().connect<&MapService::ApplyMapDiscoveries>(this);
    m_discoveryDisconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&MapService::OnDisconnected>(this);
    m_discoverySettingsConnection = aDispatcher.sink<ServerSettings>().connect<&MapService::OnDiscoverySettings>(this);
    m_playerNotifySetWaypointConnection =
        m_dispatcher.sink<NotifySetWaypoint>().connect<&MapService::OnNotifySetWaypoint>(this);
    m_playerNotifyRemoveWaypointConnection =
        m_dispatcher.sink<NotifyRemoveWaypoint>().connect<&MapService::OnNotifyRemoveWaypoint>(this);
    m_playerSetWaypointConnection =
        m_dispatcher.sink<SetWaypointEvent>().connect<&MapService::OnSetWaypoint>(this);
    m_playerRemoveWaypointConnection =
        m_dispatcher.sink<RemoveWaypointEvent>().connect<&MapService::OnRemoveWaypoint>(this);
}

void MapService::OnSetWaypoint(const SetWaypointEvent& acMessage) noexcept
{
    if (!m_transport.IsConnected())
        return;

    RequestSetWaypoint request{};
    request.Position = acMessage.Position;

    ModSystem& modSystem = m_world.Get().GetModSystem();
    if (!modSystem.GetServerModId(acMessage.WorldSpaceFormID, request.WorldSpaceFormID))
        return;

    m_transport.Send(request);
}

void MapService::OnRemoveWaypoint(const RemoveWaypointEvent& acMessage) noexcept
{
    if (!m_transport.IsConnected())
        return;

    RequestRemoveWaypoint request{};
    m_transport.Send(request);
}

void MapService::OnNotifySetWaypoint(const NotifySetWaypoint& acMessage) noexcept
{
    NiPoint3 pos{};
    pos.x = acMessage.Position.x;
    pos.y = acMessage.Position.y;
    pos.z = acMessage.Position.z;

    ModSystem& modSystem = m_world.Get().GetModSystem();
    const uint32_t cWorldSpaceID = modSystem.GetGameId(acMessage.WorldSpaceFormID);

    // The game takes the map's worldspace, or its cell on an interior map, and reads its form type without a null
    // check, so a waypoint whose location this client doesn't have must be skipped.
    TESForm* pLocation = TESForm::GetById(cWorldSpaceID);
    if (!Cast<TESWorldSpace>(pLocation) && !Cast<TESObjectCELL>(pLocation))
    {
        spdlog::warn("{}: no worldspace or cell {:X} for the waypoint, skipping it", __FUNCTION__, cWorldSpaceID);
        return;
    }

    PlayerCharacter::Get()->SetWaypoint(&pos, reinterpret_cast<TESWorldSpace*>(pLocation));
}

void MapService::OnNotifyRemoveWaypoint(const NotifyRemoveWaypoint& acMessage) noexcept
{
    PlayerCharacter::Get()->RemoveWaypoint();
}

void MapService::OnDiscoverySettings(const ServerSettings& acSettings) noexcept
{
    if (!acSettings.ShareMapDiscoveries)
    {
        if (m_pDiscoverySource)
            m_pDiscoverySource->UnRegisterSink(this);
        m_pDiscoverySource = nullptr;
        m_pendingDiscoveries.clear();
        return;
    }
    if (m_pDiscoverySource)
        return;
    // CommonLibSSE-NG LocationDiscovery::GetEventSource, AE Address Library ID.
    using TGetLocationDiscoverySource = EventDispatcher<LocationDiscoveryEvent>* (*)();
    const auto getSource = reinterpret_cast<TGetLocationDiscoverySource>(VersionDb::Get().FindAddressById(41067));
    if (getSource)
        m_pDiscoverySource = getSource();
    if (m_pDiscoverySource)
        m_pDiscoverySource->RegisterSink(this);
}

MapService::~MapService() noexcept
{
    if (m_pDiscoverySource)
        m_pDiscoverySource->UnRegisterSink(this);
}

BSTEventResult MapService::OnEvent(const LocationDiscoveryEvent* apEvent, const EventDispatcher<LocationDiscoveryEvent>*)
{
    if (apEvent && apEvent->pMarker)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(apEvent->pMarker);
        m_world.GetRunner().Queue([this, address]() { ReportDiscovery(address); });
    }
    return BSTEventResult::kOk;
}

void MapService::ReportDiscovery(uintptr_t aMarkerAddress) noexcept
{
    if (!m_pDiscoverySource || !m_transport.IsOnline())
        return;
    auto* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const auto* pMarker = FindDiscoveredMarker(pPlayer->GetParentCellEx(), aMarkerAddress);
    auto* pWorldSpace = pPlayer->GetWorldSpace();
    if (!pMarker && pWorldSpace)
        pMarker = FindDiscoveredMarker(pWorldSpace->persistentCell, aMarkerAddress);
    if (!pMarker && pWorldSpace)
    {
        const auto* pTES = TES::Get();
        auto* pMods = ModManager::Get();
        if (pTES && pMods)
        {
            constexpr int32_t radius = GridCellCoords::m_gridsToLoad / 2;
            for (int32_t dy = -radius; dy <= radius && !pMarker; ++dy)
                for (int32_t dx = -radius; dx <= radius && !pMarker; ++dx)
                {
                    auto* pCell = pMods->GetCellFromCoordinates(pTES->centerGridX + dx, pTES->centerGridY + dy, pWorldSpace, false);
                    if (pCell && pCell->IsAttached())
                        pMarker = FindDiscoveredMarker(pCell, aMarkerAddress);
                }
        }
    }

    RequestMapDiscovery request{};
    if (pMarker && m_world.GetModSystem().GetServerModId(pMarker->formID, request.Marker))
        m_transport.Send(request);
}

void MapService::OnMapDiscoveries(const NotifyMapDiscoveries& acMessage) noexcept
{
    if (!m_world.GetServerSettings().ShareMapDiscoveries)
        return;
    constexpr size_t maxPendingDiscoveries = 16384;
    for (const auto& marker : acMessage.Markers)
    {
        if (m_pendingDiscoveries.size() >= maxPendingDiscoveries)
            break;
        if (marker)
            m_pendingDiscoveries.insert(marker);
    }
}

void MapService::ApplyMapDiscoveries(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (m_pendingDiscoveries.empty() || now < m_nextDiscoveryUpdate || !m_transport.IsOnline())
        return;
    m_nextDiscoveryUpdate = now + std::chrono::seconds(1);
    const auto* pUI = UI::Get();
    if (!pUI || pUI->GetMenuOpen("Loading Menu") || pUI->GetMenuOpen("MapMenu"))
        return;

    std::erase_if(m_pendingDiscoveries, [this](const GameId& marker)
    {
        const uint32_t localId = m_world.GetModSystem().GetGameId(marker);
        auto* pReference = localId ? Cast<TESObjectREFR>(TESForm::GetById(localId)) : nullptr;
        auto* pMarker = pReference ? GetShareableMapMarker(*pReference) : nullptr;
        if (!pMarker)
            return false;
        // Match only the discovery bits. Avoid native discovery events and their rewards.
        pMarker->flags |= MapMarkerData::Visible | MapMarkerData::CanTravelTo;
        return true;
    });
}

void MapService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pendingDiscoveries.clear();
    m_nextDiscoveryUpdate = {};
    OnDiscoverySettings({});
}
