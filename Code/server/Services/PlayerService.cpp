#include "Events/CharacterInteriorCellChangeEvent.h"
#include "Events/CharacterExteriorCellChangeEvent.h"
#include "Events/PlayerLeaveCellEvent.h"
#include "Events/PlayerLeaveEvent.h"

#include <Services/PlayerService.h>
#include <Services/CharacterService.h>
#include <GameServer.h>

#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestCarriageTravel.h>
#include <Structs/CarriageTravel.h>

#include <Setting.h>
namespace
{
Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "How much of their gold players lose on death, from 0 to 1 (e.g. 0.05 loses 5%, 0 loses nothing)", 0.0f};
}

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_interiorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&PlayerService::HandleInteriorCellEnter>(this))
    , m_gridCellShiftConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&PlayerService::HandleGridCellShift>(this))
    , m_exteriorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&PlayerService::HandleExteriorCellEnter>(this))
    , m_playerRespawnConnection(aDispatcher.sink<PacketEvent<PlayerRespawnRequest>>().connect<&PlayerService::OnPlayerRespawnRequest>(this))
    , m_playerLevelConnection(aDispatcher.sink<PacketEvent<PlayerLevelRequest>>().connect<&PlayerService::OnPlayerLevelRequest>(this))
    , m_carriageTravelConnection(aDispatcher.sink<PacketEvent<RequestCarriageTravel>>().connect<&PlayerService::OnCarriageTravel>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PlayerService::OnPlayerLeave>(this))
{
}

void PlayerService::OnCarriageTravel(const PacketEvent<RequestCarriageTravel>& acMessage) noexcept
{
    const auto& request = acMessage.Packet;
    const auto& player = *acMessage.pPlayer;
    const auto& cell = player.GetCellComponent();
    const auto character = player.GetCharacter();
    const auto& mods = m_world.ctx().at<ModsComponent>();
    if (!character || !request.Token || request.Cell != cell.Cell ||
        !mods.FindFormKey(request.Seat, player) || !mods.FindFormKey(request.Cell, player) ||
        (cell.WorldSpaceId && !mods.FindFormKey(cell.WorldSpaceId, player)))
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto isNear = [](const glm::vec3& acLeft, const glm::vec3& acRight)
    {
        const auto delta = acLeft - acRight;
        return glm::dot(delta, delta) <= CarriageTravel::kBoardingDistance * CarriageTravel::kBoardingDistance;
    };
    if (!request.Arrived)
    {
        const auto* pMovement = m_world.try_get<MovementComponent>(*character);
        if (pMovement && isNear(pMovement->Position, request.Position))
            m_carriageBoardings.insert_or_assign(player.GetId(), CarriageBoarding{
                request.Seat, cell.Cell, request.Position, request.Token, now + CarriageTravel::kBoardingLifetime});
        return;
    }

    const auto departure = m_carriageBoardings.find(player.GetId());
    if (departure == m_carriageBoardings.end() || departure->second.ExpiresAt <= now ||
        departure->second.Token != request.Token || departure->second.Seat != request.Seat || departure->second.Cell == cell.Cell)
        return;

    const auto boarding = departure->second;
    m_carriageBoardings.erase(departure);
    NotifyTeleport notify{};
    notify.CellId = cell.Cell;
    notify.WorldSpaceId = cell.WorldSpaceId;
    notify.Position = request.Position;
    for (const Player* pRider : m_world.GetPlayerManager())
    {
        const auto rider = m_carriageBoardings.find(pRider->GetId());
        const auto riderCharacter = pRider->GetCharacter();
        if (rider == m_carriageBoardings.end() || !riderCharacter || rider->second.ExpiresAt <= now ||
            rider->second.Seat != boarding.Seat || rider->second.Cell != boarding.Cell ||
            pRider->GetCellComponent().Cell != boarding.Cell || !pRider->HasModId(cell.Cell.ModId) ||
            (cell.WorldSpaceId && !pRider->HasModId(cell.WorldSpaceId.ModId)))
            continue;

        const auto* pMovement = m_world.try_get<MovementComponent>(*riderCharacter);
        if (!pMovement || !isNear(pMovement->Position, boarding.Position))
            continue;

        notify.CarriageToken = rider->second.Token;
        m_carriageBoardings.erase(rider);
        pRider->Send(notify);
    }
}

void PlayerService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    m_carriageBoardings.erase(acEvent.pPlayer->GetId());
}

void SendPlayerCellChanged(const Player* apPlayer) noexcept
{
    auto& cellComponent = apPlayer->GetCellComponent();

    NotifyPlayerCellChanged notify{};
    notify.PlayerId = apPlayer->GetId();
    notify.WorldSpaceId = cellComponent.WorldSpaceId;
    notify.CellId = cellComponent.Cell;

    GameServer::Get()->SendToPlayers(notify, apPlayer);
}

void PlayerService::HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const GameId oldCell = pPlayer->GetCellComponent().Cell;

    CellIdComponent cell = CellIdComponent{message.PlayerCell, message.WorldSpaceId, message.CenterCoords};
    pPlayer->SetCellComponent(cell);

    m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));

    auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto character : characterView)
    {
        const auto& ownedComponent = characterView.get<OwnerComponent>(character);
        const auto& characterCellComponent = characterView.get<CellIdComponent>(character);

        if (ownedComponent.GetOwner() == pPlayer)
            continue;

        const auto& characterComponent = characterView.get<CharacterComponent>(character);
        // An actor's parent cell can be temporary and absent from the client's cell list.
        // Use the same worldspace/position range as movement broadcasts, including dragon range.
        const bool isInRange = cell.IsInRange(characterCellComponent, characterComponent.IsDragon());

        if (!isInRange)
        {
            continue;
        }

        CharacterSpawnRequest spawnMessage;
        CharacterService::Serialize(m_world, character, &spawnMessage);

        pPlayer->Send(spawnMessage);
    }
}

void PlayerService::HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    auto* pPlayer = acMessage.pPlayer;

    const GameId oldCell = pPlayer->GetCellComponent().Cell;
    pPlayer->SetCellComponent({message.CellId, message.WorldSpaceId, message.CurrentCoords});
    if (const auto character = pPlayer->GetCharacter())
        m_world.GetDispatcher().trigger(CharacterExteriorCellChangeEvent{pPlayer, *character, message.CellId, message.WorldSpaceId, message.CurrentCoords});
    if (oldCell && oldCell != message.CellId)
        m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));
    SendPlayerCellChanged(pPlayer);
}

void PlayerService::HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const auto oldCell = pPlayer->GetCellComponent().Cell;

    auto cell = CellIdComponent{message.CellId, {}, {}};
    pPlayer->SetCellComponent(cell);

    if (oldCell && oldCell != message.CellId)
        m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));

    if (pPlayer->GetCharacter())
    {
        auto entity = *pPlayer->GetCharacter();

        if (auto pCellIdComponent = m_world.try_get<CellIdComponent>(entity); pCellIdComponent)
        {
            m_world.GetDispatcher().trigger(CharacterInteriorCellChangeEvent{pPlayer, entity, message.CellId});
        }
    }

    auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    for (auto character : characterView)
    {
        const auto& ownedComponent = characterView.get<OwnerComponent>(character);

        if (ownedComponent.GetOwner() == pPlayer)
            continue;

        if (message.CellId != characterView.get<CellIdComponent>(character).Cell)
            continue;

        CharacterSpawnRequest spawnMessage;
        CharacterService::Serialize(m_world, character, &spawnMessage);

        pPlayer->Send(spawnMessage);
    }

    SendPlayerCellChanged(pPlayer);
}

void PlayerService::OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept
{
    float goldLossFactor = fGoldLossFactor.as_float();

    auto character = acMessage.pPlayer->GetCharacter();
    if (!character)
        return;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(*character));

    if (it != view.end())
    {
        if (goldLossFactor != 0.0)
        {
            auto& inventoryComponent = view.get<InventoryComponent>(*it);

            GameId goldId(0, 0xF);
            int32_t goldCount = inventoryComponent.Content.GetEntryCountById(goldId);
            int32_t goldToRemove = static_cast<int32_t>(goldCount * goldLossFactor);

            Inventory::Entry entry{};
            entry.BaseId = goldId;
            entry.Count = -goldToRemove;

            inventoryComponent.Content.AddOrRemoveEntry(entry);

            NotifyInventoryChanges notifyInventoryChanges{};
            notifyInventoryChanges.ServerId = World::ToInteger(*character);
            if (const auto* pOwnerComponent = m_world.try_get<OwnerComponent>(*character))
                notifyInventoryChanges.OwnershipEpoch = pOwnerComponent->OwnershipEpoch;
            notifyInventoryChanges.Item = entry;
            notifyInventoryChanges.Drop = false;

            // Exclude respawned player from inventory changes notification...
            if (!GameServer::Get()->SendToPlayersInRange(notifyInventoryChanges, *character, acMessage.GetSender()))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);

            // ...and instead, send NotifyPlayerRespawn so that the client can print a message.
            NotifyPlayerRespawn notifyPlayerRespawn{};
            notifyPlayerRespawn.GoldLost = goldToRemove;

            acMessage.pPlayer->Send(notifyPlayerRespawn);
        }

        // Let all other players in cell respawn this player, since the body state seems to be bugged otherwise
        NotifyRespawn notifyRespawn{};
        notifyRespawn.ActorId = World::ToInteger(*character);

        if (!GameServer::Get()->SendToPlayersInRange(notifyRespawn, *character, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
}

void PlayerService::OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept
{
    acMessage.pPlayer->SetLevel(acMessage.Packet.NewLevel);

    NotifyPlayerLevel notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.NewLevel = acMessage.Packet.NewLevel;

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}
