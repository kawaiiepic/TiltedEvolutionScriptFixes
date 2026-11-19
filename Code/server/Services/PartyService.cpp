#include <Services/PartyService.h>
#include <Components.h>
#include <GameServer.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyInvite.h>
#include <Messages/PartyInviteRequest.h>
#include <Messages/PartyAcceptInviteRequest.h>
#include <Messages/PartyLeaveRequest.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyPartyLeft.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyChangeLeaderRequest.h>
#include <Messages/PartyKickRequest.h>
#include <Messages/NotifyPlayerJoined.h>

PartyService::PartyService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_updateEvent(aDispatcher.sink<UpdateEvent>().connect<&PartyService::OnUpdate>(this))
    , m_playerJoinConnection(aDispatcher.sink<PlayerJoinEvent>().connect<&PartyService::OnPlayerJoin>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&PartyService::OnPlayerLeave>(this))
    , m_partyInviteConnection(aDispatcher.sink<PacketEvent<PartyInviteRequest>>().connect<&PartyService::OnPartyInvite>(this))
    , m_partyAcceptInviteConnection(aDispatcher.sink<PacketEvent<PartyAcceptInviteRequest>>().connect<&PartyService::OnPartyAcceptInvite>(this))
    , m_partyLeaveConnection(aDispatcher.sink<PacketEvent<PartyLeaveRequest>>().connect<&PartyService::OnPartyLeave>(this))
    , m_partyCreateConnection(aDispatcher.sink<PacketEvent<PartyCreateRequest>>().connect<&PartyService::OnPartyCreate>(this))
    , m_partyChangeLeaderConnection(aDispatcher.sink<PacketEvent<PartyChangeLeaderRequest>>().connect<&PartyService::OnPartyChangeLeader>(this))
    , m_partyKickConnection(aDispatcher.sink<PacketEvent<PartyKickRequest>>().connect<&PartyService::OnPartyKick>(this))
{
}

const PartyService::Party* PartyService::GetById(uint32_t aId) const noexcept
{
    auto itor = m_parties.find(aId);
    if (itor != std::end(m_parties))
        return &itor->second;

    return nullptr;
}

bool PartyService::IsPlayerInParty(Player* const apPlayer) const noexcept
{
    return apPlayer->GetParty().JoinedPartyId.has_value();
}

bool PartyService::IsPlayerLeader(const Player* const apPlayer) const noexcept
{
    const auto& inviterPartyComponent = apPlayer->GetParty();
    if (inviterPartyComponent.JoinedPartyId)
    {
        if (const auto* const pParty = GetById(*inviterPartyComponent.JoinedPartyId))
            return pParty->LeaderPlayerId == apPlayer->GetId();
    }

    return false;
}

PartyService::Party* PartyService::GetPlayerParty(Player* const apPlayer) noexcept
{
    auto& inviterPartyComponent = apPlayer->GetParty();
    if (inviterPartyComponent.JoinedPartyId)
    {
        return &m_parties[*inviterPartyComponent.JoinedPartyId];
    }

    return nullptr;
}

// Legacy management packets remain decodable but cannot change membership.
void PartyService::OnUpdate(const UpdateEvent&) noexcept {}
void PartyService::OnPartyCreate(const PacketEvent<PartyCreateRequest>&) noexcept {}
void PartyService::OnPartyChangeLeader(const PacketEvent<PartyChangeLeaderRequest>&) noexcept {}
void PartyService::OnPartyKick(const PacketEvent<PartyKickRequest>&) noexcept {}

void PartyService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    BroadcastPlayerList();

    NotifyPlayerJoined notify{};
    notify.PlayerId = acEvent.pPlayer->GetId();
    notify.Username = acEvent.pPlayer->GetUsername();

    notify.WorldSpaceId = acEvent.WorldSpaceId;
    notify.CellId = acEvent.CellId;

    notify.Level = acEvent.pPlayer->GetLevel();

    spdlog::debug("[Party] New notify player {:x} {}", notify.PlayerId, notify.Username.c_str());

    GameServer::Get()->SendToPlayers(notify, acEvent.pPlayer);

    auto* player = acEvent.pPlayer;
    auto& party = m_parties[kGlobalPartyId];
    if (std::find(party.Members.begin(), party.Members.end(), player) == party.Members.end())
        party.Members.push_back(player);
    if (party.Members.size() == 1) party.LeaderPlayerId = player->GetId();
    player->GetParty().JoinedPartyId = kGlobalPartyId;
    SendPartyJoinedEvent(party, player);
    BroadcastPartyInfo(kGlobalPartyId);
}

void PartyService::OnPartyInvite(const PacketEvent<PartyInviteRequest>&) noexcept {}
void PartyService::OnPartyAcceptInvite(const PacketEvent<PartyAcceptInviteRequest>&) noexcept {}
void PartyService::OnPartyLeave(const PacketEvent<PartyLeaveRequest>&) noexcept {}

void PartyService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    RemovePlayerFromParty(acEvent.pPlayer);
    BroadcastPlayerList(acEvent.pPlayer);
}

void PartyService::RemovePlayerFromParty(Player* apPlayer) noexcept
{
    auto* pPartyComponent = &apPlayer->GetParty();
    spdlog::debug("[PartyService]: Removing player from party.");

    if (pPartyComponent->JoinedPartyId)
    {
        auto id = *pPartyComponent->JoinedPartyId;

        Party& party = m_parties[id];
        auto& members = party.Members;

        const auto member = std::find(members.begin(), members.end(), apPlayer);
        if (member != members.end()) members.erase(member);

        if (members.empty())
        {
            m_parties.erase(id);
        }
        else
        {
            if (party.LeaderPlayerId == apPlayer->GetId())
            {
                party.LeaderPlayerId = members.at(0)->GetId(); // Reassign party leader
                spdlog::debug("[PartyService]: Leader left, reassigned party leader to {}", party.LeaderPlayerId);
            }
            spdlog::debug("[PartyService]: Updating other party players of removal.");
            BroadcastPartyInfo(id);
        }

        pPartyComponent->JoinedPartyId.reset();

        spdlog::debug("[PartyService]: Sending party left event to player.");
        NotifyPartyLeft leftMessage;
        apPlayer->Send(leftMessage);
    }
}

void PartyService::BroadcastPlayerList(Player* apPlayer) const noexcept
{
    auto pIgnoredPlayer = apPlayer;
    for (auto pSelf : m_world.GetPlayerManager())
    {
        if (pIgnoredPlayer == pSelf)
            continue;

        NotifyPlayerList playerList;
        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pSelf == pPlayer)
                continue;

            if (pIgnoredPlayer == pPlayer)
                continue;

            playerList.Players[pPlayer->GetId()] = pPlayer->GetUsername();
        }

        pSelf->Send(playerList);
    }
}

void PartyService::BroadcastPartyInfo(uint32_t aPartyId) const noexcept
{
    auto itor = m_parties.find(aPartyId);
    if (itor == std::end(m_parties))
        return;

    auto& party = itor->second;
    auto& members = party.Members;

    NotifyPartyInfo message;
    message.LeaderPlayerId = party.LeaderPlayerId;

    for (auto pPlayer : members)
    {
        message.PlayerIds.push_back(pPlayer->GetId());
    }

    for (auto pPlayer : members)
    {
        message.IsLeader = pPlayer->GetId() == party.LeaderPlayerId;
        pPlayer->Send(message);
    }
}

void PartyService::SendPartyJoinedEvent(Party& aParty, Player* aPlayer) noexcept
{
    NotifyPartyJoined joinedMessage;
    joinedMessage.LeaderPlayerId = aParty.LeaderPlayerId;
    joinedMessage.IsLeader = aParty.LeaderPlayerId == aPlayer->GetId();
    for (auto pPlayer : aParty.Members)
    {
        joinedMessage.PlayerIds.push_back(pPlayer->GetId());
    }
    spdlog::debug("[PartyService]: Sending party join event to player");
    aPlayer->Send(joinedMessage);
}
