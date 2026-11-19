#include <GameServer.h>
#include <Components.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <algorithm>
#include <iterator>

#include <Services/OverlayService.h>
#include <Services/DialogueLocation.h>
#include <Setting.h>

#include <ChatMessageTypes.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Messages/TeleportRequest.h>
#include <Messages/NotifyTeleport.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include "Game/Player.h"

#include <regex>


namespace
{
Console::Setting bRemoteDialogueSpectating{"Gameplay:bRemoteDialogueSpectating", "Allow remote listening to public conversations in the same worldspace or interior", false};

void SendDialogueDenied(const Player& acPlayer, uint32_t aActorId, uint32_t aToken)
{
    NotifyPlayerDialogue denied{};
    denied.Kind = NotifyPlayerDialogue::Denied;
    denied.ActorId = aActorId;
    denied.Token = aToken;
    acPlayer.Send(denied);
}
}

OverlayService::OverlayService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_chatMessageConnection = aDispatcher.sink<PacketEvent<SendChatMessageRequest>>().connect<&OverlayService::HandleChatMessage>(this);
    m_playerDialogueConnection = aDispatcher.sink<PacketEvent<PlayerDialogueRequest>>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_dialogueUpdateConnection = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::OnDialogueUpdate>(this);
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
    const auto& playerCell = acPlayer.GetCellComponent();
    if (!pCharacter || pCharacter->IsPlayer() || pCharacter->IsDead() || !pCell ||
        !DialogueLocation::IsCompatible(pCell->Cell, pCell->WorldSpaceId, playerCell.Cell, playerCell.WorldSpaceId) ||
        !pCell->IsInRange(playerCell, false))
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

uint32_t OverlayService::GetPublicDialogueOwner(const GameId& acReference) const noexcept
{
    for (const auto& [actorId, session] : m_dialogueSessions)
    {
        if (session.Private)
            continue;
        const auto* pForm = m_world.try_get<FormIdComponent>(static_cast<entt::entity>(actorId));
        const auto* pOwner = m_world.GetPlayerManager().GetById(session.Owner);
        if (pForm && pForm->Id == acReference && pOwner && IsDialogueActorInRange(actorId, *pOwner))
            return session.Owner;
    }
    return 0;
}

bool OverlayService::CanRelayDialogue(const Player& acPlayer, uint32_t aActorId, uint32_t aToken) const noexcept
{
    const auto session = m_dialogueSessions.find(aActorId);
    if (aToken)
    {
        if (session == m_dialogueSessions.end() || session->second.Private ||
            session->second.Owner != acPlayer.GetId() || !IsDialogueActorInRange(aActorId, acPlayer))
            return false;

        const auto participant = session->second.Participants.find(acPlayer.GetId());
        return participant != session->second.Participants.end() && participant->second == aToken;
    }

    // An unrelated actor can still speak during a private conversation.
    if (session != m_dialogueSessions.end() && session->second.Private)
        return false;

    const auto* pOwner = m_world.try_get<OwnerComponent>(static_cast<entt::entity>(aActorId));
    return pOwner && pOwner->GetOwner() == &acPlayer;
}

void OverlayService::SendConversationLine(const ServerMessage& acMessage, uint32_t aActorId, const Player& acSpeaker) const noexcept
{
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != &acSpeaker && IsDialogueActorInRange(aActorId, *pPlayer))
            pPlayer->Send(acMessage);
    }
}

bool OverlayService::CanListen(uint32_t aActorId, const DialogueSession& acSession, const Player& acPlayer) const noexcept
{
    if (!bRemoteDialogueSpectating || acSession.Private || !acPlayer.GetCharacter() || acPlayer.GetId() == acSession.Owner)
        return false;
    const auto* pOwner = m_world.GetPlayerManager().GetById(acSession.Owner);
    if (!pOwner || !IsDialogueActorInRange(aActorId, *pOwner))
        return false;
    const auto& speaker = pOwner->GetCellComponent();
    const auto& listener = acPlayer.GetCellComponent();
    return DialogueLocation::IsCompatible(speaker.Cell, speaker.WorldSpaceId, listener.Cell, listener.WorldSpaceId);
}

void OverlayService::RecordConversationSubtitle(uint32_t aActorId, const String& acText) noexcept
{
    const auto session = m_dialogueSessions.find(aActorId);
    if (session == m_dialogueSessions.end() || !bRemoteDialogueSpectating || session->second.Private)
        return;
    auto& state = session->second;
    state.Subtitle = acText.size() <= 2048 ? acText : String{};
    state.SubtitleExpires = std::chrono::steady_clock::now() + std::chrono::seconds(8);
}

void OverlayService::InvalidateOptions(DialogueSession& aSession) noexcept
{
    aSession.Options.clear();
    aSession.Subtitle.clear();
    aSession.Votes.clear();
    if (++m_nextDialogueRevision == 0)
        ++m_nextDialogueRevision;
    aSession.Revision = m_nextDialogueRevision;
}

void OverlayService::OnDialogueUpdate(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextPanelRefresh)
        return;
    m_nextPanelRefresh = now + std::chrono::milliseconds(500);

    // Revoke a grant only by ending the session, never by giving the same live
    // native menu grant to two players. A revoked client must reopen its menu.
    for (auto it = m_dialogueSessions.begin(); it != m_dialogueSessions.end();)
    {
        const auto* pOwner = m_world.GetPlayerManager().GetById(it->second.Owner);
        if (pOwner && IsDialogueActorInRange(it->first, *pOwner))
        {
            ++it;
            continue;
        }
        for (const auto& [playerId, token] : it->second.Participants)
        {
            if (const auto* pPlayer = m_world.GetPlayerManager().GetById(playerId))
                SendDialogueDenied(*pPlayer, it->first, token);
            m_playerDialogues.erase(playerId);
        }
        it = m_dialogueSessions.erase(it);
    }
    RefreshDialoguePanels();
}

void OverlayService::RefreshDialoguePanels() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& [actorId, session] : m_dialogueSessions)
    {
        if (now >= session.SubtitleExpires)
            session.Subtitle.clear();
        for (auto it = session.Listeners.begin(); it != session.Listeners.end();)
        {
            const auto* pListener = m_world.GetPlayerManager().GetById(*it);
            if (!pListener || !CanListen(actorId, session, *pListener))
            {
                session.Votes.erase(*it);
                it = session.Listeners.erase(it);
            }
            else
                ++it;
        }
        for (auto it = session.Votes.begin(); it != session.Votes.end();)
        {
            const auto* pVoter = m_world.GetPlayerManager().GetById(it->first);
            if (!pVoter || (!session.Listeners.contains(it->first) && !IsDialogueActorInRange(actorId, *pVoter)))
                it = session.Votes.erase(it);
            else
                ++it;
        }
    }

    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        auto& previous = m_visiblePanels[pPlayer->GetId()];
        std::map<uint32_t, NotifyPlayerDialogue> visible;
        for (const auto& [actorId, session] : m_dialogueSessions)
        {
            const bool cOwner = pPlayer->GetId() == session.Owner;
            const bool cNearby = IsDialogueActorInRange(actorId, *pPlayer);
            if (session.Private && !cOwner)
                continue;
            const auto* pOwner = m_world.GetPlayerManager().GetById(session.Owner);
            if (!pOwner || !IsDialogueActorInRange(actorId, *pOwner))
                continue;
            const bool cListening = session.Listeners.contains(pPlayer->GetId());
            if (!cNearby && !CanListen(actorId, session, *pPlayer))
                continue;
            NotifyPlayerDialogue panel{};
            panel.Kind = NotifyPlayerDialogue::Panel;
            panel.ActorId = actorId;
            const auto* pForm = m_world.try_get<FormIdComponent>(static_cast<entt::entity>(actorId));
            if (pForm && pPlayer->HasModId(pForm->Id.ModId))
                panel.SpeakerReference = pForm->Id;
            panel.Revision = session.Revision;
            panel.CanChoose = cOwner;
            panel.CanVote = (cNearby || cListening) && !cOwner;
            panel.Remote = !cNearby || cListening;
            panel.Listening = cListening;
            panel.CanPass = cOwner && std::any_of(session.Participants.begin(), session.Participants.end(),
                [&](const auto& participant)
                {
                    const auto* pCandidate = m_world.GetPlayerManager().GetById(participant.first);
                    return participant.first != session.Owner && pCandidate && IsDialogueActorInRange(actorId, *pCandidate);
                });
            panel.Private = session.Private;
            panel.Name = pOwner->GetUsername();
            if (cNearby || cListening)
                panel.Options = session.Options;
            if (cListening && !cNearby)
                panel.Text = session.Subtitle;
            panel.Votes.resize(panel.Options.size(), 0);
            for (const auto& [voter, index] : session.Votes)
            {
                if (index < panel.Votes.size())
                {
                    ++panel.Votes[index];
                    if (!session.Private && panel.VoterChoices.size() < NotifyPlayerDialogue::MaxVoterChoices)
                        panel.VoterChoices.emplace_back(voter, index);
                }
                if (voter == pPlayer->GetId())
                    panel.Selected = index;
            }
            const auto old = previous.find(actorId);
            if (old == previous.end() || !(old->second == panel))
                pPlayer->Send(panel);
            visible.emplace(actorId, std::move(panel));
        }
        for (const auto& [actorId, panel] : previous)
        {
            if (visible.contains(actorId))
                continue;
            NotifyPlayerDialogue closed{};
            closed.Kind = NotifyPlayerDialogue::PanelClosed;
            closed.ActorId = actorId;
            pPlayer->Send(closed);
        }
        previous = std::move(visible);
    }
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
    state.Votes.erase(aPlayerId);
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
                SendDialogueDenied(*pPlayer, cActorId, it->second);
            m_playerDialogues.erase(it->first);
            it = state.Participants.erase(it);
        }

        if (state.Participants.empty())
        {
            m_dialogueSessions.erase(session);
            return;
        }
        state.Owner = state.Participants.begin()->first;
        InvalidateOptions(state);
    }

    SendDialogueState(cActorId, state);
}

void OverlayService::SetDialoguePrivacy(uint32_t aActorId, DialogueSession& aSession, bool aPrivate) noexcept
{
    if (aSession.Private == aPrivate)
        return;

    aSession.Private = aPrivate;
    aSession.Votes.clear();
    aSession.Subtitle.clear();
    if (aPrivate)
    {
        aSession.Listeners.clear();
        for (auto it = aSession.Participants.begin(); it != aSession.Participants.end();)
        {
            if (it->first == aSession.Owner)
            {
                ++it;
                continue;
            }
            if (const auto* pPlayer = m_world.GetPlayerManager().GetById(it->first))
                SendDialogueDenied(*pPlayer, aActorId, it->second);
            m_playerDialogues.erase(it->first);
            it = aSession.Participants.erase(it);
        }
    }
    // Old panel votes must not become valid if the owner switches back to public.
    if (++m_nextDialogueRevision == 0)
        ++m_nextDialogueRevision;
    aSession.Revision = m_nextDialogueRevision;
    RefreshDialoguePanels();
}

void OverlayService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    LeaveDialogue(acEvent.pPlayer->GetId());
    m_visiblePanels.erase(acEvent.pPlayer->GetId());
    for (auto& [actorId, session] : m_dialogueSessions)
    {
        session.Votes.erase(acEvent.pPlayer->GetId());
        session.Listeners.erase(acEvent.pPlayer->GetId());
    }
}

void OverlayService::OnPlayerDialogue(const PacketEvent<PlayerDialogueRequest>& acMessage) noexcept
{
    const auto& request = acMessage.Packet;
    Player* pPlayer = acMessage.pPlayer;
    const uint32_t cPlayerId = pPlayer->GetId();

    if (request.Action > PlayerDialogueRequest::StopListening || request.Text.size() > 1024 || request.Options.size() > 64)
        return;

    if (request.Action == PlayerDialogueRequest::Listen || request.Action == PlayerDialogueRequest::StopListening)
    {
        const auto session = m_dialogueSessions.find(request.ActorId);
        if (session == m_dialogueSessions.end())
            return;
        auto& state = session->second;
        if (request.Action == PlayerDialogueRequest::StopListening)
        {
            if (!state.Listeners.erase(cPlayerId))
                return;
            state.Votes.erase(cPlayerId);
        }
        else
        {
            if (request.Revision != state.Revision || !CanListen(request.ActorId, state, *pPlayer) ||
                IsDialogueActorInRange(request.ActorId, *pPlayer))
                return;
            for (auto& [actorId, other] : m_dialogueSessions)
                if (actorId != request.ActorId && other.Listeners.erase(cPlayerId))
                    other.Votes.erase(cPlayerId);
            state.Listeners.insert(cPlayerId);
        }
        RefreshDialoguePanels();
        return;
    }

    if (request.Action == PlayerDialogueRequest::Vote)
    {
        const auto session = m_dialogueSessions.find(request.ActorId);
        if (session == m_dialogueSessions.end())
            return;
        auto& state = session->second;
        const auto* pOwner = m_world.GetPlayerManager().GetById(state.Owner);
        if (!pOwner || !IsDialogueActorInRange(request.ActorId, *pOwner))
            return;
        if (!IsDialogueActorInRange(request.ActorId, *pPlayer) &&
            (!state.Listeners.contains(cPlayerId) || !CanListen(request.ActorId, state, *pPlayer)))
            return;
        if (state.Private || state.Owner == cPlayerId || request.Revision != state.Revision || request.Index >= state.Options.size() ||
            state.Options[request.Index].empty())
            return;
        state.Votes[cPlayerId] = request.Index;
        return;
    }

    if (!request.Token)
        return;

    if (request.Action == PlayerDialogueRequest::Join)
    {
        const auto existing = m_dialogueSessions.find(request.ActorId);
        if (!IsDialogueActorInRange(request.ActorId, *pPlayer) ||
            (existing != m_dialogueSessions.end() && existing->second.Private && existing->second.Owner != cPlayerId))
        {
            SendDialogueDenied(*pPlayer, request.ActorId, request.Token);
            return;
        }

        const auto current = m_playerDialogues.find(cPlayerId);
        if (current != m_playerDialogues.end() && current->second == request.ActorId)
        {
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
        {
            state.Owner = cPlayerId;
            InvalidateOptions(state);
        }

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

    if (!IsDialogueActorInRange(request.ActorId, *pPlayer))
        return;

    const bool cOwner = state.Owner == cPlayerId;
    if (request.Action == PlayerDialogueRequest::SetPrivacy)
    {
        if (cOwner && request.Index <= 1)
            SetDialoguePrivacy(request.ActorId, state, request.Index != 0);
        return;
    }
    if (request.Action == PlayerDialogueRequest::PublishOptions)
    {
        if (!cOwner || std::any_of(request.Options.begin(), request.Options.end(), [](const auto& option) { return option.size() > 1024; }))
            return;
        if (state.Options != request.Options)
        {
            InvalidateOptions(state);
            state.Options = request.Options;
        }
        return;
    }
    if (request.Action == PlayerDialogueRequest::Suggest && request.Text.empty())
        return;
    if ((request.Action == PlayerDialogueRequest::Choose && !cOwner) ||
        (request.Action == PlayerDialogueRequest::Suggest && cOwner))
    {
        SendDialogueState(request.ActorId, state);
        return;
    }

    if (!cOwner)
    {
        // Native menus may show a different set of options. Match only an exact,
        // unambiguous owner option; otherwise retain the suggestion as chat.
        const auto option = std::find(state.Options.begin(), state.Options.end(), request.Text);
        if (option != state.Options.end() && std::find(std::next(option), state.Options.end(), request.Text) == state.Options.end())
            state.Votes[cPlayerId] = static_cast<uint32_t>(std::distance(state.Options.begin(), option));
    }
    else
    {
        InvalidateOptions(state);
        if (request.Text.empty())
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
        if (const auto* pRecipient = m_world.GetPlayerManager().GetById(playerId);
            pRecipient && IsDialogueActorInRange(request.ActorId, *pRecipient))
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
