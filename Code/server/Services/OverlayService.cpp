#include <GameServer.h>
#include <Components.h>
#include <Events/PlayerLeaveEvent.h>

#include <Services/OverlayService.h>

#include <ChatMessageTypes.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/TeleportRequest.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include "Game/Player.h"

#include <regex>


OverlayService::OverlayService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_chatMessageConnection = aDispatcher.sink<PacketEvent<SendChatMessageRequest>>().connect<&OverlayService::HandleChatMessage>(this);
    m_playerDialogueConnection = aDispatcher.sink<PacketEvent<PlayerDialogueRequest>>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_dialogueLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&OverlayService::OnPlayerLeave>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportRequest>>().connect<&OverlayService::OnTeleport>(this);
    m_playerHealthConnection = aDispatcher.sink<PacketEvent<RequestPlayerHealthUpdate>>().connect<&OverlayService::OnPlayerHealthUpdate>(this);
}

void sendPlayerMessage(const ChatMessageType acType, const String acContent, Player* aSendingPlayer) noexcept
{
    NotifyChatMessageBroadcast notifyMessage{};

    std::regex escapeHtml{"<[^>]+>\\s+(?=<)|<[^>]+>"};
    notifyMessage.MessageType = acType;
    notifyMessage.PlayerName = std::regex_replace(aSendingPlayer->GetUsername(), escapeHtml, "");
    notifyMessage.ChatMessage = std::regex_replace(acContent, escapeHtml, "");

    auto character = aSendingPlayer->GetCharacter();

    switch (notifyMessage.MessageType)
    {
    case kGlobalChat: GameServer::Get()->SendToPlayers(notifyMessage); break;

    case kSystemMessage: spdlog::error("PlayerId {} attempted to send a System Message.", aSendingPlayer->GetId()); break;

    case kPlayerDialogue: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kPartyChat: GameServer::Get()->SendToParty(notifyMessage, aSendingPlayer->GetParty()); break;

    case kLocalChat:
        if (character)
        {
            if (!GameServer::Get()->SendToPlayersInRange(notifyMessage, *character))
                spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
        }
        break;
    default: spdlog::error("{} is not a known MessageType", static_cast<uint64_t>(notifyMessage.MessageType)); break;
    }
}

void OverlayService::HandleChatMessage(const PacketEvent<SendChatMessageRequest>& acMessage) const noexcept
{
    auto [canceled, reason] = m_world.GetScriptService().HandleChatMessage(*acMessage.pPlayer->GetCharacter(), acMessage.Packet.ChatMessage);
    if (canceled)
        return;

    sendPlayerMessage(acMessage.Packet.MessageType, acMessage.Packet.ChatMessage, acMessage.pPlayer);
}

bool OverlayService::IsDialogueActorInRange(uint32_t aActorId, const Player& acPlayer) const noexcept
{
    const auto entity = static_cast<entt::entity>(aActorId);
    if (!m_world.valid(entity) || !acPlayer.GetCharacter())
        return false;

    const auto* pCharacter = m_world.try_get<CharacterComponent>(entity);
    const auto* pCell = m_world.try_get<CellIdComponent>(entity);
    if (!pCharacter || pCharacter->IsPlayer() || pCharacter->IsDead() || !pCell ||
        !pCell->IsInRange(acPlayer.GetCellComponent(), false))
        return false;

    const auto* pActorMovement = m_world.try_get<MovementComponent>(entity);
    const auto* pPlayerMovement = m_world.try_get<MovementComponent>(*acPlayer.GetCharacter());
    if (pActorMovement && pPlayerMovement)
    {
        const auto delta = pActorMovement->Position - pPlayerMovement->Position;
        return glm::dot(delta, delta) <= 2000.f * 2000.f;
    }
    return true;
}

void OverlayService::SendDialogueState(uint32_t aActorId, const DialogueSession& acSession) const noexcept
{
    const auto* pOwner = m_world.GetPlayerManager().GetById(acSession.Owner);
    if (!pOwner)
        return;

    NotifyPlayerDialogue notify{};
    notify.Kind = NotifyPlayerDialogue::State;
    notify.ActorId = aActorId;
    notify.Name = pOwner->GetUsername();
    for (const auto& [playerId, token] : acSession.Participants)
    {
        if (const auto* pPlayer = m_world.GetPlayerManager().GetById(playerId))
        {
            notify.Token = token;
            notify.CanChoose = playerId == acSession.Owner;
            pPlayer->Send(notify);
        }
    }
}

void OverlayService::LeaveDialogue(uint32_t aPlayerId) noexcept
{
    const auto member = m_playerDialogues.find(aPlayerId);
    if (member == m_playerDialogues.end())
        return;

    const uint32_t cActorId = member->second;
    m_playerDialogues.erase(member);
    const auto session = m_dialogueSessions.find(cActorId);
    if (session == m_dialogueSessions.end())
        return;

    auto& state = session->second;
    state.Participants.erase(aPlayerId);
    if (state.Participants.empty())
    {
        m_dialogueSessions.erase(session);
        return;
    }

    if (state.Owner == aPlayerId)
    {
        // Only transfer after the old owner has released the menu (or disconnected).
        // Never steal a live grant on a timer while its client could still select.
        for (auto it = state.Participants.begin(); it != state.Participants.end();)
        {
            const auto* pPlayer = m_world.GetPlayerManager().GetById(it->first);
            if (pPlayer && IsDialogueActorInRange(cActorId, *pPlayer))
            {
                ++it;
                continue;
            }

            if (pPlayer)
            {
                NotifyPlayerDialogue denied{};
                denied.Kind = NotifyPlayerDialogue::Denied;
                denied.ActorId = cActorId;
                denied.Token = it->second;
                pPlayer->Send(denied);
            }
            m_playerDialogues.erase(it->first);
            it = state.Participants.erase(it);
        }

        if (state.Participants.empty())
        {
            m_dialogueSessions.erase(session);
            return;
        }
        state.Owner = state.Participants.begin()->first;
    }

    SendDialogueState(cActorId, state);
}

void OverlayService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    LeaveDialogue(acEvent.pPlayer->GetId());
}

void OverlayService::OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) noexcept
{
    const auto& request = acMessage.Packet;
    Player* pPlayer = acMessage.pPlayer;
    const uint32_t cPlayerId = pPlayer->GetId();

    if (!request.Token || request.Action > PlayerDialogueRequest::Choose || request.Text.size() > 1024)
        return;

    if (request.Action == PlayerDialogueRequest::Join)
    {
        if (!IsDialogueActorInRange(request.ActorId, *pPlayer))
        {
            NotifyPlayerDialogue denied{};
            denied.Kind = NotifyPlayerDialogue::Denied;
            denied.ActorId = request.ActorId;
            denied.Token = request.Token;
            pPlayer->Send(denied);
            return;
        }

        const auto current = m_playerDialogues.find(cPlayerId);
        if (current != m_playerDialogues.end() && current->second == request.ActorId)
        {
            const auto existing = m_dialogueSessions.find(request.ActorId);
            if (existing != m_dialogueSessions.end())
            {
                auto& state = existing->second;
                const auto participant = state.Participants.find(cPlayerId);
                if (participant != state.Participants.end() && participant->second == request.Token)
                {
                    SendDialogueState(request.ActorId, state);
                    return;
                }
            }
        }

        LeaveDialogue(cPlayerId);
        auto& state = m_dialogueSessions[request.ActorId];
        if (state.Participants.empty())
            state.Owner = cPlayerId;

        state.Participants[cPlayerId] = request.Token;
        m_playerDialogues[cPlayerId] = request.ActorId;
        SendDialogueState(request.ActorId, state);
        return;
    }

    const auto member = m_playerDialogues.find(cPlayerId);
    const auto session = m_dialogueSessions.find(request.ActorId);
    if (member == m_playerDialogues.end() || member->second != request.ActorId || session == m_dialogueSessions.end())
        return;

    auto& state = session->second;
    const auto participant = state.Participants.find(cPlayerId);
    if (participant == state.Participants.end() || participant->second != request.Token)
        return;

    if (request.Action == PlayerDialogueRequest::Leave)
    {
        LeaveDialogue(cPlayerId);
        return;
    }

    if (!IsDialogueActorInRange(request.ActorId, *pPlayer) || request.Text.empty())
        return;

    const bool cOwner = state.Owner == cPlayerId;
    if ((request.Action == PlayerDialogueRequest::Choose && !cOwner) ||
        (request.Action == PlayerDialogueRequest::Suggest && cOwner))
    {
        SendDialogueState(request.ActorId, state);
        return;
    }

    // The selection hook only executes the native response for the granted owner.
    // Suggestions are explanatory chat text and never execute a game function here.
    NotifyPlayerDialogue notify{};
    notify.Kind = cOwner ? NotifyPlayerDialogue::Choice : NotifyPlayerDialogue::Suggestion;
    notify.ActorId = request.ActorId;
    const std::regex escapeHtml{"<[^>]+>\\s+(?=<)|<[^>]+>"};
    notify.Name = std::regex_replace(pPlayer->GetUsername(), escapeHtml, "");
    notify.Text = std::regex_replace(request.Text, escapeHtml, "");
    for (const auto& [playerId, token] : state.Participants)
    {
        if (!cOwner && playerId != state.Owner && playerId != cPlayerId)
            continue;
        if (const auto* pRecipient = m_world.GetPlayerManager().GetById(playerId))
        {
            notify.Token = token;
            pRecipient->Send(notify);
        }
    }
}

void OverlayService::OnTeleport(const PacketEvent<TeleportRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = m_world.GetPlayerManager().GetById(acMessage.Packet.PlayerId);
    if (!pTargetPlayer)
        return;

    NotifyTeleport response{};

    auto character = pTargetPlayer->GetCharacter();
    if (character)
    {
        const auto* pMovementComponent = m_world.try_get<MovementComponent>(*character);
        if (pMovementComponent)
        {
            const auto& cellComponent = pTargetPlayer->GetCellComponent();
            response.CellId = cellComponent.Cell;
            response.Position = pMovementComponent->Position;
            response.WorldSpaceId = cellComponent.WorldSpaceId;
        }
    }

    acMessage.pPlayer->Send(response);
}

void OverlayService::OnPlayerHealthUpdate(const PacketEvent<RequestPlayerHealthUpdate>& acMessage) const noexcept
{
    NotifyPlayerHealthUpdate notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.Percentage = acMessage.Packet.Percentage;

    GameServer::Get()->SendToParty(notify, acMessage.pPlayer->GetParty(), acMessage.GetSender());
}
