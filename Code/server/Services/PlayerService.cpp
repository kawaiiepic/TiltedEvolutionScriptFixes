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
#include <Messages/NotifyTravelInvitation.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <ChatMessageTypes.h>
#include <Messages/RequestCarriageTravel.h>
#include <Messages/RequestFastTravelCatchUp.h>
#include <Messages/RequestTravelResult.h>
#include <Structs/CarriageTravel.h>
#include <Structs/PlayerRevive.h>

#include <algorithm>
#include <cmath>
#include <charconv>
#include <Setting.h>

namespace
{
constexpr auto kTravelInviteDuration = std::chrono::seconds(10);
constexpr auto kTravelRetryDuration = std::chrono::seconds(30);
constexpr float kDefaultGoldLossFactor = 0.10f;
using RecoveryState = CharacterComponent::RecoveryState;

Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "Gold lost on self-respawn. Values up to 1 are fractions of carried gold, values above 1 are flat amounts, and 0 disables the fee.", kDefaultGoldLossFactor};

Console::Setting bCarriageCatchUp{"Gameplay:bCarriageCatchUp", "Allow opt-in catch-up travel after native carriage arrivals", false};

Console::Setting bFastTravelCatchUp{"Gameplay:bFastTravelCatchUp", "Allow opt-in catch-up after completed map fast travel", false};

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
    , m_fastTravelCatchUpConnection(aDispatcher.sink<PacketEvent<RequestFastTravelCatchUp>>().connect<&PlayerService::OnFastTravelCatchUp>(this))
    , m_travelResultConnection(aDispatcher.sink<PacketEvent<RequestTravelResult>>().connect<&PlayerService::OnTravelResult>(this))
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

    std::set<uint32_t> alreadyTraveling;
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
        if (bCarriageCatchUp)
            alreadyTraveling.insert(pRider->GetId());
    }
    if (bCarriageCatchUp)
        OfferCatchUp(player, request.Position, alreadyTraveling, false);
}

void PlayerService::OnFastTravelCatchUp(const PacketEvent<RequestFastTravelCatchUp>& acMessage) noexcept
{
    const auto& player = *acMessage.pPlayer;
    const auto& request = acMessage.Packet;
    const auto& cell = player.GetCellComponent();
    if (!bFastTravelCatchUp || !player.GetCharacter() || !request.Sequence || !request.Origin ||
        request.Origin == request.Destination || request.Destination != cell.Cell || !cell.WorldSpaceId ||
        !m_world.ctx().at<ModsComponent>().FindFormKey(request.Origin, player))
        return;

    const auto departure = m_cellDepartures.find(player.GetId());
    if (departure == m_cellDepartures.end() || departure->second.Cell != request.Origin ||
        std::chrono::steady_clock::now() - departure->second.Time > std::chrono::seconds(15))
        return;

    auto& sequence = m_fastTravelSequences[player.GetId()];
    if (request.Sequence <= sequence)
        return;
    sequence = request.Sequence;
    m_cellDepartures.erase(departure);
    OfferCatchUp(player, request.Position, {}, true);
}

void PlayerService::OfferCatchUp(const Player& acLeader, const glm::vec3& acPosition,
                                 const std::set<uint32_t>& acAlreadyTraveling, bool aFastTravel) noexcept
{
    const uint32_t leaderId = acLeader.GetId();
    if (m_travelDispatches.contains(leaderId) ||
        std::ranges::any_of(m_travelInvitations, [leaderId](const auto& entry) { return entry.second.Leader == leaderId; }) ||
        !std::isfinite(acPosition.x) || !std::isfinite(acPosition.y) || !std::isfinite(acPosition.z))
        return;

    const auto& cell = acLeader.GetCellComponent();
    const auto& mods = m_world.ctx().at<ModsComponent>();
    const auto destination = mods.FindFormKey(cell.Cell, acLeader);
    if (!destination || (cell.WorldSpaceId && !mods.FindFormKey(cell.WorldSpaceId, acLeader)))
        return;

    TravelInvitation invitation{};
    invitation.Leader = leaderId;
    invitation.FastTravel = aFastTravel;
    invitation.Cell = cell.Cell;
    invitation.WorldSpace = cell.WorldSpaceId;
    invitation.Position = acPosition;
    invitation.ExpiresAt = std::chrono::steady_clock::now() + kTravelInviteDuration;
    for (const Player* pPlayer : m_world.GetPlayerManager())
        if (!acAlreadyTraveling.contains(pPlayer->GetId()) && CanJoinTravel(*pPlayer, invitation))
            invitation.Invited.insert(pPlayer->GetId());
    if (invitation.Invited.empty())
        return;
    invitation.Id = NextTravelInvitationId();
    for (auto& [id, other] : m_travelInvitations)
        if (other.Accepted.erase(leaderId))
            PublishTravelInvitation(other);

    const auto message = fmt::format("{} {} at {}:{:X}. Type /travel join {} within 10 seconds to catch up.",
                                     acLeader.GetUsername().c_str(), aFastTravel ? "finished fast travel" : "arrived by carriage",
                                     destination->Plugin, destination->BaseId, invitation.Id);
    for (const uint32_t id : invitation.Invited)
        if (const auto* pPlayer = m_world.GetPlayerManager().GetById(id))
            SendTravelMessage(*pPlayer, message);
    SendTravelMessage(acLeader, fmt::format("Catch-up invitation sent. Type /travel cancel {} to cancel.", invitation.Id));
    const auto inserted = m_travelInvitations.emplace(invitation.Id, std::move(invitation));
    PublishTravelInvitation(inserted.first->second);
}

uint32_t PlayerService::NextTravelInvitationId() noexcept
{
    do
    {
        ++m_nextTravelId;
    }
    while (!m_nextTravelId || m_travelInvitations.contains(m_nextTravelId));
    return m_nextTravelId;
}

bool PlayerService::CanJoinTravel(const Player& acPlayer, const TravelInvitation& acInvitation) const noexcept
{
    if (m_travelDispatches.contains(acPlayer.GetId()) || acPlayer.GetId() == acInvitation.Leader || acPlayer.GetCellComponent().Cell == acInvitation.Cell ||
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
    if (idText.empty() || (action != "join" && action != "decline" && action != "cancel" && action != "retry"))
    {
        SendTravelMessage(acPlayer, "Use /travel join ID, /travel decline ID, /travel cancel ID, or /travel retry ID.");
        return true;
    }

    uint32_t id{};
    const auto result = std::from_chars(idText.data(), idText.data() + idText.size(), id);
    if (result.ec != std::errc{} || result.ptr != idText.data() + idText.size() || !id)
    {
        SendTravelMessage(acPlayer, "That travel invitation is no longer open.");
        return true;
    }
    if (action == "retry")
    {
        RetryTravel(acPlayer, id);
        return true;
    }
    const auto offer = m_travelInvitations.find(id);
    if (offer == m_travelInvitations.end() || std::chrono::steady_clock::now() >= offer->second.ExpiresAt)
    {
        SendTravelMessage(acPlayer, "That travel invitation is no longer open.");
        return true;
    }

    auto& invitation = offer->second;
    const uint32_t playerId = acPlayer.GetId();
    if (action == "cancel" && playerId == invitation.Leader)
    {
        EndTravelInvitation(invitation, "The traveler canceled the invitation.");
        m_travelInvitations.erase(offer);
    }
    else if (action == "cancel" || !invitation.Invited.contains(playerId))
        SendTravelMessage(acPlayer, "You cannot change that travel invitation.");
    else if (action == "decline")
    {
        if (invitation.Accepted.erase(playerId))
            PublishTravelInvitation(invitation);
        SendTravelMessage(acPlayer, "You will stay here.");
    }
    else if (std::ranges::any_of(m_travelInvitations, [&](const auto& entry)
    {
        return entry.first != id && (entry.second.Leader == playerId || entry.second.Accepted.contains(playerId));
    }))
        SendTravelMessage(acPlayer, "Leave your other travel invitation before joining this one.");
    else if (CanJoinTravel(acPlayer, invitation))
    {
        if (invitation.Accepted.insert(playerId).second)
            PublishTravelInvitation(invitation);
        SendTravelMessage(acPlayer, fmt::format("Accepted. {} travelers have joined.", invitation.Accepted.size()));
    }
    else
        SendTravelMessage(acPlayer, "You cannot travel to that destination.");
    return true;
}

void PlayerService::RetryTravel(const Player& acPlayer, uint32_t aPreviousId) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    const uint32_t playerId = acPlayer.GetId();
    const auto retry = m_travelRetries.find(playerId);
    if (retry == m_travelRetries.end() || retry->second.Id != aPreviousId || now >= retry->second.Deadline)
    {
        SendTravelMessage(acPlayer, "That travel retry is no longer available.");
        return;
    }

    const auto& attempt = retry->second;
    const auto* pLeader = m_world.GetPlayerManager().GetById(attempt.Leader);
    TravelInvitation invitation{};
    invitation.Leader = attempt.Leader;
    invitation.FastTravel = attempt.FastTravel;
    invitation.Cell = attempt.Cell;
    invitation.WorldSpace = attempt.WorldSpace;
    invitation.Position = attempt.Position;
    if ((attempt.FastTravel ? !bFastTravelCatchUp : !bCarriageCatchUp) || !pLeader ||
        pLeader->GetCellComponent().Cell != invitation.Cell || pLeader->GetCellComponent().WorldSpaceId != invitation.WorldSpace ||
        !IsTravelLeaderReady(*pLeader, invitation.Position))
    {
        m_travelRetries.erase(retry);
        SendTravelMessage(acPlayer, "Travel cannot be retried because the destination is no longer available.");
        return;
    }

    if (!CanJoinTravel(acPlayer, invitation) || m_travelDispatches.contains(attempt.Leader) ||
        std::ranges::any_of(m_travelInvitations, [&](const auto& entry)
        {
            const auto& other = entry.second;
            return other.Leader == playerId || other.Leader == attempt.Leader ||
                   other.Accepted.contains(playerId) || other.Accepted.contains(attempt.Leader);
        }))
    {
        SendTravelMessage(acPlayer, "Travel cannot be retried while either traveler is busy.");
        return;
    }

    invitation.Id = NextTravelInvitationId();
    invitation.ExpiresAt = now + kTravelInviteDuration;
    invitation.Invited.insert(playerId);
    invitation.Accepted.insert(playerId);
    m_travelRetries.erase(retry);
    const auto inserted = m_travelInvitations.emplace(invitation.Id, std::move(invitation));
    PublishTravelInvitation(inserted.first->second);
    SendTravelMessage(acPlayer, fmt::format("Travel retry accepted. Type /travel decline {} to stay here.", inserted.first->first));
}

void PlayerService::OnTravelResult(const PacketEvent<RequestTravelResult>& acMessage) noexcept
{
    const auto& result = acMessage.Packet;
    const auto& player = *acMessage.pPlayer;
    const auto dispatch = m_travelDispatches.find(player.GetId());
    if (dispatch == m_travelDispatches.end() || result.Id != dispatch->second.Id ||
        result.Outcome > RequestTravelResult::Interrupted ||
        std::chrono::steady_clock::now() >= dispatch->second.Deadline)
        return;

    const auto& cell = player.GetCellComponent();
    if (result.Outcome == RequestTravelResult::Arrived &&
        (cell.Cell != dispatch->second.Cell || cell.WorldSpaceId != dispatch->second.WorldSpace))
        return;

    if (result.Outcome == RequestTravelResult::Busy || result.Outcome == RequestTravelResult::Failed)
    {
        auto retry = dispatch->second;
        retry.Deadline = std::chrono::steady_clock::now() + kTravelRetryDuration;
        m_travelRetries.insert_or_assign(player.GetId(), retry);
        SendTravelMessage(player, fmt::format("Type /travel retry {} within 30 seconds to retry.", retry.Id));
    }
    m_travelDispatches.erase(dispatch);
    switch (result.Outcome)
    {
    case RequestTravelResult::Arrived: SendTravelMessage(player, "Travel arrival confirmed."); break;
    case RequestTravelResult::Busy: SendTravelMessage(player, "Travel skipped while your character was busy."); break;
    case RequestTravelResult::Interrupted: SendTravelMessage(player, "Travel was interrupted by another transition."); break;
    default: SendTravelMessage(player, "Travel could not be completed."); break;
    }
}

void PlayerService::UpdateTravelInvitation(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_travelRetries, [now](const auto& entry) { return now >= entry.second.Deadline; });
    std::erase_if(m_travelDispatches, [&](const auto& entry)
    {
        if (now < entry.second.Deadline)
            return false;
        if (const auto* pPlayer = m_world.GetPlayerManager().GetById(entry.first))
            SendTravelMessage(*pPlayer, "Travel arrival could not be confirmed.");
        return true;
    });

    std::erase_if(m_travelInvitations, [&](const auto& entry)
    {
        return UpdateTravelOffer(entry.second, now);
    });
}

bool PlayerService::IsTravelLeaderReady(const Player& acLeader, const glm::vec3& acPosition) const noexcept
{
    const auto character = acLeader.GetCharacter();
    const auto* pMovement = character ? m_world.try_get<MovementComponent>(*character) : nullptr;
    const auto* pState = character ? m_world.try_get<CharacterComponent>(*character) : nullptr;
    return pMovement && pState && !pState->IsDead() && pState->Recovery == RecoveryState::Standing &&
           glm::distance(pMovement->Position, acPosition) <= CarriageTravel::kBoardingDistance;
}

bool PlayerService::UpdateTravelOffer(const TravelInvitation& acInvitation, std::chrono::steady_clock::time_point aNow) noexcept
{
    const auto& invitation = acInvitation;
    const auto* pLeader = m_world.GetPlayerManager().GetById(invitation.Leader);
    if ((invitation.FastTravel ? !bFastTravelCatchUp : !bCarriageCatchUp) || !pLeader ||
        pLeader->GetCellComponent().Cell != invitation.Cell ||
        pLeader->GetCellComponent().WorldSpaceId != invitation.WorldSpace)
    {
        EndTravelInvitation(invitation, "Travel canceled because the destination is no longer active.");
        return true;
    }
    if (aNow < invitation.ExpiresAt)
        return false;

    if (!IsTravelLeaderReady(*pLeader, invitation.Position))
    {
        EndTravelInvitation(invitation, "Travel canceled because the traveler is no longer at the arrival point.");
        return true;
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
            {
                m_travelRetries.erase(id);
                m_travelDispatches.emplace(id, TravelDispatch{invitation.Id, invitation.Leader, invitation.FastTravel,
                    invitation.Cell, invitation.WorldSpace, invitation.Position, aNow + std::chrono::seconds(20)});
                pRider->Send(notify);
            }
            else
                SendTravelMessage(*pRider, "Travel skipped because you are no longer eligible.");
        }
    PublishTravelInvitation(invitation, true);
    return true;
}

void PlayerService::PublishTravelInvitation(const TravelInvitation& invitation, bool aClosed) const noexcept
{
    const auto& players = m_world.GetPlayerManager();
    const auto* pLeader = players.GetById(invitation.Leader);
    NotifyTravelInvitation notify{};
    notify.Id = invitation.Id;
    if (!aClosed)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            invitation.ExpiresAt - std::chrono::steady_clock::now()).count();
        notify.RemainingMs = static_cast<uint32_t>(std::clamp<int64_t>(remaining, 1, 10000));
        notify.ParticipantCount = static_cast<uint32_t>(invitation.Accepted.size()) + 1;
        notify.CellId = invitation.Cell;
        notify.WorldSpaceId = invitation.WorldSpace;
        if (pLeader)
            notify.LeaderName = pLeader->GetUsername();
    }
    for (const uint32_t id : invitation.Invited)
        if (const auto* pPlayer = players.GetById(id))
        {
            notify.Accepted = !aClosed && invitation.Accepted.contains(id);
            pPlayer->Send(notify);
        }
    if (pLeader)
    {
        notify.CanCancel = !aClosed;
        notify.Accepted = false;
        pLeader->Send(notify);
    }
}

void PlayerService::EndTravelInvitation(const TravelInvitation& acInvitation, std::string_view aMessage) const noexcept
{
    PublishTravelInvitation(acInvitation, true);
    for (const uint32_t id : acInvitation.Invited)
        if (const auto* pPlayer = m_world.GetPlayerManager().GetById(id))
            SendTravelMessage(*pPlayer, aMessage);
}

void PlayerService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    const uint32_t playerId = acEvent.pPlayer->GetId();
    m_carriageBoardings.erase(playerId);
    m_fastTravelSequences.erase(playerId);
    m_travelDispatches.erase(playerId);
    m_travelRetries.erase(playerId);
    std::erase_if(m_travelRetries, [playerId](const auto& entry) { return entry.second.Leader == playerId; });
    m_cellDepartures.erase(playerId);
    for (auto it = m_travelInvitations.begin(); it != m_travelInvitations.end();)
    {
        auto& invitation = it->second;
        if (invitation.Leader == playerId)
        {
            EndTravelInvitation(invitation, "Travel canceled because the traveler disconnected.");
            it = m_travelInvitations.erase(it);
            continue;
        }
        invitation.Invited.erase(playerId);
        if (invitation.Accepted.erase(playerId))
            PublishTravelInvitation(invitation);
        ++it;
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

void PlayerService::HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) noexcept
{
    auto* pPlayer = acMessage.pPlayer;

    auto& message = acMessage.Packet;

    const GameId oldCell = pPlayer->GetCellComponent().Cell;

    CellIdComponent cell = CellIdComponent{message.PlayerCell, message.WorldSpaceId, message.CenterCoords};
    if (bFastTravelCatchUp && oldCell && oldCell != cell.Cell)
        m_cellDepartures.insert_or_assign(pPlayer->GetId(), CellDeparture{oldCell, std::chrono::steady_clock::now()});
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

void PlayerService::HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;
    auto* pPlayer = acMessage.pPlayer;

    const GameId oldCell = pPlayer->GetCellComponent().Cell;
    if (bFastTravelCatchUp && oldCell && oldCell != message.CellId)
        m_cellDepartures.insert_or_assign(pPlayer->GetId(), CellDeparture{oldCell, std::chrono::steady_clock::now()});
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

    const auto& previousLocation = pPlayer->GetCellComponent();
    const auto oldCell = previousLocation.Cell;
    const auto& mods = m_world.ctx().at<ModsComponent>();
    const bool linkedDoor = message.SourceDoor && mods.FindFormKey(message.SourceDoor, *pPlayer);
    if (oldCell != message.CellId && !linkedDoor)
    {
        pPlayer->InteriorEntrance = {};
        pPlayer->EntranceWorldSpace = {};
    }
    if (previousLocation.WorldSpaceId && message.CellId && linkedDoor &&
        mods.FindFormKey(previousLocation.WorldSpaceId, *pPlayer))
    {
        const auto character = pPlayer->GetCharacter();
        const auto* pMovement = character ? m_world.try_get<MovementComponent>(*character) : nullptr;
        constexpr float kEntranceDistance = 1024.f;
        if (pMovement && glm::distance(pMovement->Position, message.SourceDoorPosition) <= kEntranceDistance)
        {
            pPlayer->InteriorEntrance = message.SourceDoor;
            pPlayer->EntranceWorldSpace = previousLocation.WorldSpaceId;
            pPlayer->EntrancePosition = message.SourceDoorPosition;
        }
    }

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
