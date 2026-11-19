#include "Events/CharacterInteriorCellChangeEvent.h"
#include "Events/CharacterExteriorCellChangeEvent.h"
#include "Events/PlayerLeaveCellEvent.h"
#include "Events/PlayerLeaveEvent.h"
#include "Events/PlayerJoinEvent.h"
#include "Events/UpdateEvent.h"

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
#include <Messages/NotifyChatMessageBroadcast.h>
#include <ChatMessageTypes.h>
#include <Messages/RequestCarriageTravel.h>
#include <Structs/CarriageTravel.h>
#include <Structs/PlayerRevive.h>

#include <algorithm>
#include <cmath>
#include <charconv>
#include <Setting.h>

namespace
{
constexpr auto kTravelInviteDuration = std::chrono::seconds(10);
constexpr float kDefaultGoldLossFactor = 0.10f;
using RecoveryState = CharacterComponent::RecoveryState;

Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "Gold lost on self-respawn. Values up to 1 are fractions of carried gold, values above 1 are flat amounts, and 0 disables the fee.", kDefaultGoldLossFactor};

Console::Setting bCarriageCatchUp{"Gameplay:bCarriageCatchUp", "Allow opt-in catch-up travel after native carriage arrivals", false};

void SendTravelMessage(const Player& acPlayer, std::string_view aMessage)
{
    NotifyChatMessageBroadcast notify{};
    notify.MessageType = kSystemMessage;
    notify.ChatMessage.assign(aMessage.begin(), aMessage.end());
    acPlayer.Send(notify);
}

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
    , m_travelUpdateConnection(aDispatcher.sink<UpdateEvent>().connect<&PlayerService::UpdateTravelInvitation>(this))
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

    std::optional<TravelInvitation> invitation;
    if (bCarriageCatchUp && !m_travelInvitation && std::isfinite(request.Position.x) &&
        std::isfinite(request.Position.y) && std::isfinite(request.Position.z))
    {
        if (++m_nextTravelId == 0)
            ++m_nextTravelId;
        invitation.emplace();
        invitation->Id = m_nextTravelId;
        invitation->Leader = player.GetId();
        invitation->Cell = cell.Cell;
        invitation->WorldSpace = cell.WorldSpaceId;
        invitation->Position = request.Position;
        invitation->ExpiresAt = now + kTravelInviteDuration;
    }
    for (const Player* pRider : m_world.GetPlayerManager())
    {
        const auto rider = m_carriageBoardings.find(pRider->GetId());
        const auto riderCharacter = pRider->GetCharacter();
        if (invitation && CanJoinTravel(*pRider, *invitation))
            invitation->Invited.insert(pRider->GetId());
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
        if (invitation)
            invitation->Invited.erase(pRider->GetId());
    }
    if (!invitation || invitation->Invited.empty())
        return;

    const auto destination = mods.FindFormKey(cell.Cell, player);
    const auto message = fmt::format("{} arrived by carriage at {}:{:X}. Type /travel join {} within 10 seconds to catch up.",
                                     player.GetUsername().c_str(), destination->Plugin, destination->BaseId, invitation->Id);
    for (const uint32_t id : invitation->Invited)
        if (const auto* pRider = m_world.GetPlayerManager().GetById(id))
            SendTravelMessage(*pRider, message);
    SendTravelMessage(player, fmt::format("Catch-up invitation sent. Type /travel cancel {} to cancel.", invitation->Id));
    m_travelInvitation = std::move(invitation);
}

bool PlayerService::CanJoinTravel(const Player& acPlayer, const TravelInvitation& acInvitation) const noexcept
{
    if (acPlayer.GetId() == acInvitation.Leader || acPlayer.GetCellComponent().Cell == acInvitation.Cell ||
        !acPlayer.HasModId(acInvitation.Cell.ModId) ||
        (acInvitation.WorldSpace && !acPlayer.HasModId(acInvitation.WorldSpace.ModId)))
        return false;
    const auto character = acPlayer.GetCharacter();
    const auto* pState = character ? m_world.try_get<CharacterComponent>(*character) : nullptr;
    return pState && !pState->IsDead() && pState->Recovery == RecoveryState::Standing;
}

bool PlayerService::HandleTravelCommand(const Player& acPlayer, std::string_view aCommand) noexcept
{
    if (aCommand != "/travel" && !aCommand.starts_with("/travel "))
        return false;

    aCommand.remove_prefix(std::string_view("/travel").size());
    if (aCommand.starts_with(' '))
        aCommand.remove_prefix(1);
    const auto separator = aCommand.find(' ');
    const auto action = aCommand.substr(0, separator);
    const auto idText = separator == aCommand.npos ? std::string_view{} : aCommand.substr(separator + 1);
    if (idText.empty() || (action != "join" && action != "decline" && action != "cancel"))
    {
        SendTravelMessage(acPlayer, "Use /travel join ID, /travel decline ID, or /travel cancel ID.");
        return true;
    }

    uint32_t id{};
    const auto result = std::from_chars(idText.data(), idText.data() + idText.size(), id);
    if (result.ec != std::errc{} || result.ptr != idText.data() + idText.size() ||
        !m_travelInvitation || m_travelInvitation->Id != id ||
        std::chrono::steady_clock::now() >= m_travelInvitation->ExpiresAt)
    {
        SendTravelMessage(acPlayer, "That travel invitation is no longer open.");
        return true;
    }

    auto& invitation = *m_travelInvitation;
    const uint32_t playerId = acPlayer.GetId();
    if (action == "cancel" && playerId == invitation.Leader)
        EndTravelInvitation("The carriage traveler canceled the invitation.");
    else if (action == "cancel" || !invitation.Invited.contains(playerId))
        SendTravelMessage(acPlayer, "You cannot change that travel invitation.");
    else if (action == "decline")
    {
        invitation.Accepted.erase(playerId);
        SendTravelMessage(acPlayer, "You will stay here.");
    }
    else if (CanJoinTravel(acPlayer, invitation))
    {
        invitation.Accepted.insert(playerId);
        SendTravelMessage(acPlayer, fmt::format("Accepted. {} travelers have joined.", invitation.Accepted.size()));
    }
    else
        SendTravelMessage(acPlayer, "You cannot travel to that destination.");
    return true;
}

void PlayerService::UpdateTravelInvitation(const UpdateEvent&) noexcept
{
    if (!m_travelInvitation)
        return;

    const auto& invitation = *m_travelInvitation;
    const auto* pLeader = m_world.GetPlayerManager().GetById(invitation.Leader);
    if (!bCarriageCatchUp || !pLeader || pLeader->GetCellComponent().Cell != invitation.Cell ||
        pLeader->GetCellComponent().WorldSpaceId != invitation.WorldSpace)
    {
        EndTravelInvitation("Travel canceled because the destination is no longer active.");
        return;
    }
    if (std::chrono::steady_clock::now() < invitation.ExpiresAt)
        return;

    const auto character = pLeader->GetCharacter();
    const auto* pMovement = character ? m_world.try_get<MovementComponent>(*character) : nullptr;
    const auto* pState = character ? m_world.try_get<CharacterComponent>(*character) : nullptr;
    if (!pMovement || !pState || pState->IsDead() || pState->Recovery != RecoveryState::Standing ||
        !(glm::distance(pMovement->Position, invitation.Position) <= CarriageTravel::kBoardingDistance))
    {
        EndTravelInvitation("Travel canceled because the carriage traveler is no longer at the arrival point.");
        return;
    }

    NotifyTeleport notify{};
    notify.CellId = invitation.Cell;
    notify.WorldSpaceId = invitation.WorldSpace;
    notify.Position = invitation.Position;
    notify.GroupTravelId = invitation.Id;
    for (const uint32_t id : invitation.Accepted)
        if (const auto* pRider = m_world.GetPlayerManager().GetById(id))
        {
            if (CanJoinTravel(*pRider, invitation))
                pRider->Send(notify);
            else
                SendTravelMessage(*pRider, "Travel skipped because you are no longer eligible.");
        }
    m_travelInvitation.reset();
}

void PlayerService::EndTravelInvitation(std::string_view aMessage) noexcept
{
    for (const uint32_t id : m_travelInvitation->Invited)
        if (const auto* pPlayer = m_world.GetPlayerManager().GetById(id))
            SendTravelMessage(*pPlayer, aMessage);
    m_travelInvitation.reset();
}

void PlayerService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const uint32_t playerId = acEvent.pPlayer->GetId();
    m_carriageBoardings.erase(playerId);
    if (m_travelInvitation)
    {
        if (m_travelInvitation->Leader == playerId)
            EndTravelInvitation("Travel canceled because the carriage traveler disconnected.");
        else
        {
            m_travelInvitation->Invited.erase(playerId);
            m_travelInvitation->Accepted.erase(playerId);
        }
    }
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
