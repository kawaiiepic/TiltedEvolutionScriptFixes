#include "Events/CharacterInteriorCellChangeEvent.h"
#include "Events/CharacterExteriorCellChangeEvent.h"
#include "Events/PlayerLeaveCellEvent.h"
#include "Events/PlayerLeaveEvent.h"
#include "Events/PlayerJoinEvent.h"

#include <Services/PlayerService.h>
#include <Services/CharacterService.h>
#include <GameServer.h>

#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyPlayerDowned.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestCarriageTravel.h>
#include <Structs/CarriageTravel.h>
#include <Structs/PlayerRevive.h>

#include <algorithm>
#include <cmath>
#include <Setting.h>

namespace
{
constexpr float kDefaultGoldLossFactor = 0.10f;
using RecoveryState = CharacterComponent::RecoveryState;

Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "Gold lost on self-respawn. Values up to 1 are fractions of carried gold, values above 1 are flat amounts, and 0 disables the fee.", kDefaultGoldLossFactor};

void BroadcastDowned(entt::entity aCharacter, uint32_t aDownId, const Player* apExcept = nullptr) noexcept
{
    NotifyPlayerDowned notify{};
    notify.ActorId = World::ToInteger(aCharacter);
    notify.DownId = aDownId;
    GameServer::Get()->SendToPlayers(notify, apExcept);
}
}

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_gridCellShiftConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&PlayerService::HandleGridCellShift>(this))
    , m_exteriorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&PlayerService::HandleExteriorCellEnter>(this))
    , m_interiorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&PlayerService::HandleInteriorCellEnter>(this))
    , m_playerRespawnConnection(aDispatcher.sink<PacketEvent<PlayerRespawnRequest>>().connect<&PlayerService::OnPlayerRespawnRequest>(this))
    , m_playerLevelConnection(aDispatcher.sink<PacketEvent<PlayerLevelRequest>>().connect<&PlayerService::OnPlayerLevelRequest>(this))
    , m_carriageTravelConnection(aDispatcher.sink<PacketEvent<RequestCarriageTravel>>().connect<&PlayerService::OnCarriageTravel>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PlayerService::OnPlayerLeave>(this))
    , m_playerJoinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&PlayerService::OnPlayerJoin>(this))
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
    if (const auto character = acEvent.pPlayer->GetCharacter())
    {
        auto* pCharacter = m_world.try_get<CharacterComponent>(*character);
        if (pCharacter && pCharacter->Recovery == RecoveryState::Downed)
        {
            pCharacter->Recovery = RecoveryState::Standing;
            BroadcastDowned(*character, 0, acEvent.pPlayer);
        }
    }
}

void PlayerService::OnPlayerJoin(const PlayerJoinEvent& acEvent) const noexcept
{
    if (!GameServer::Get()->HasDeathSystem())
        return;

    const auto characters = m_world.view<const CharacterComponent>();
    for (const auto entity : characters)
    {
        const auto& character = characters.get<const CharacterComponent>(entity);
        if (character.Recovery == RecoveryState::Downed)
        {
            NotifyPlayerDowned notify{};
            notify.ActorId = World::ToInteger(entity);
            notify.DownId = character.DownId;
            acEvent.pPlayer->Send(notify);
        }
    }
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
    const auto& request = acMessage.Packet;
    const Player& sender = *acMessage.pPlayer;
    const auto entity = sender.GetCharacter();
    if (!entity || !request.DownId)
        return;
    if (!GameServer::Get()->HasDeathSystem() && request.Action != PlayerRespawnRequest::Recovered)
        return;

    auto* pSenderCharacter = m_world.try_get<CharacterComponent>(*entity);
    const auto* pSenderOwner = m_world.try_get<OwnerComponent>(*entity);
    if (!pSenderCharacter || !pSenderCharacter->IsPlayer() || !pSenderOwner || pSenderOwner->GetOwner() != &sender)
        return;

    if (request.Action == PlayerRespawnRequest::Revive)
    {
        const auto target = static_cast<entt::entity>(request.TargetId);
        if (target == *entity || pSenderCharacter->IsDead() || pSenderCharacter->Recovery != RecoveryState::Standing)
            return;

        auto* pDowned = m_world.try_get<CharacterComponent>(target);
        const auto* pOwner = m_world.try_get<OwnerComponent>(target);
        const Player* pTargetPlayer = pOwner ? pOwner->GetOwner() : nullptr;
        if (!pDowned || !pDowned->IsPlayer() || pDowned->Recovery != RecoveryState::Downed || pDowned->DownId != request.DownId ||
            !pTargetPlayer || pTargetPlayer->GetCharacter() != target)
            return;

        const auto& senderCell = sender.GetCellComponent();
        const auto& targetCell = pTargetPlayer->GetCellComponent();
        if (!senderCell.Cell || senderCell.Cell != targetCell.Cell || senderCell.WorldSpaceId != targetCell.WorldSpaceId)
            return;

        const auto* pSenderMove = m_world.try_get<MovementComponent>(*entity);
        const auto* pTargetMove = m_world.try_get<MovementComponent>(target);
        if (!pSenderMove || !pTargetMove)
            return;
        const auto delta = pSenderMove->Position - pTargetMove->Position;
        if (!(glm::dot(delta, delta) <= PlayerRevive::kDistance * PlayerRevive::kDistance))
            return;

        pDowned->Recovery = RecoveryState::Recovering;
        GrantRecovery(*pTargetPlayer, target, request.DownId, true);
        return;
    }

    switch (request.Action)
    {
    case PlayerRespawnRequest::Downed:
        if (pSenderCharacter->Recovery != RecoveryState::Standing || request.DownId <= pSenderCharacter->DownId)
            return;
        pSenderCharacter->DownId = request.DownId;
        pSenderCharacter->DownedAt = std::chrono::steady_clock::now();
        pSenderCharacter->Recovery = RecoveryState::Downed;
        BroadcastDowned(*entity, request.DownId);
        break;
    case PlayerRespawnRequest::Respawn:
        if (pSenderCharacter->Recovery != RecoveryState::Downed || pSenderCharacter->DownId != request.DownId ||
            std::chrono::steady_clock::now() - pSenderCharacter->DownedAt < std::chrono::seconds(GameServer::GetPlayerRespawnDelay()))
            return;
        pSenderCharacter->Recovery = RecoveryState::Recovering;
        GrantRecovery(sender, *entity, request.DownId, false);
        break;
    case PlayerRespawnRequest::Recovered:
        if (pSenderCharacter->Recovery == RecoveryState::Standing || pSenderCharacter->DownId != request.DownId)
            return;
        if (pSenderCharacter->Recovery == RecoveryState::Downed)
            BroadcastDowned(*entity, 0);
        pSenderCharacter->Recovery = RecoveryState::Standing;
        if (auto* pAnimation = m_world.try_get<AnimationComponent>(*entity))
            pAnimation->ActionsReplayCache.Clear();

        // Refresh observers after the owner has left native bleedout.
        {
            NotifyRespawn notify{};
            notify.ActorId = World::ToInteger(*entity);
            notify.OwnershipEpoch = pSenderOwner->OwnershipEpoch;
            GameServer::Get()->SendToPlayersInRange(notify, *entity, &sender);
        }
        break;
    default:
        break;
    }
}

void PlayerService::GrantRecovery(const Player& acPlayer, entt::entity aCharacter, uint32_t aDownId, bool aRevived) const noexcept
{
    NotifyPlayerRespawn grant{};
    grant.DownId = aDownId;
    grant.Revived = aRevived;

    const double cGoldLossFactor = fGoldLossFactor.as_float();
    if (!aRevived && std::isfinite(cGoldLossFactor) && cGoldLossFactor > 0.0)
    {
        const auto& mods = m_world.ctx().at<ModsComponent>();
        auto goldId = mods.FindFormId({"Skyrim.esm", false, 0xF});
        auto* pInventory = m_world.try_get<InventoryComponent>(aCharacter);
        if (goldId && pInventory)
        {
            const int32_t cGoldCount = pInventory->Content.GetEntryCountById(*goldId);
            if (cGoldCount > 0)
            {
                const double cLoss = cGoldLossFactor <= 1.0 ? cGoldCount * cGoldLossFactor : cGoldLossFactor;
                grant.GoldLost = std::max(1, static_cast<int32_t>(std::min<double>(cGoldCount, cLoss)));
                NotifyInventoryChanges notify{};
                notify.ServerId = World::ToInteger(aCharacter);
                notify.OwnershipEpoch = m_world.get<OwnerComponent>(aCharacter).OwnershipEpoch;
                notify.Item.BaseId = *goldId;
                notify.Item.Count = -grant.GoldLost;
                pInventory->Content.AddOrRemoveEntry(notify.Item);
                GameServer::Get()->SendToPlayersInRange(notify, aCharacter, &acPlayer);
            }
        }
    }

    BroadcastDowned(aCharacter, 0);
    acPlayer.Send(grant);
}

void PlayerService::OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept
{
    acMessage.pPlayer->SetLevel(acMessage.Packet.NewLevel);

    NotifyPlayerLevel notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.NewLevel = acMessage.Packet.NewLevel;

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}
