#include <Services/CommandService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/SetTimeCommandRequest.h>
#include <Messages/NotifySetTimeResult.h>
#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>

CommandService::CommandService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_setTimeConnection = aDispatcher.sink<PacketEvent<SetTimeCommandRequest>>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<PacketEvent<TeleportCommandRequest>>().connect<&CommandService::OnTeleportCommandRequest>(this);
}

void CommandService::OnSetTimeCommand(const PacketEvent<SetTimeCommandRequest>& acMessage) const noexcept
{
    NotifySetTimeResult response{};

    const auto* pServer = GameServer::Get();
    const bool cIsAdmin = pServer->GetAdminSessions().contains(acMessage.pPlayer->GetConnectionId());
    const bool cIsPrivatePartyLeader = !pServer->IsPublicServer() && m_world.GetPartyService().IsPlayerLeader(acMessage.pPlayer);
    if (!cIsAdmin && !cIsPrivatePartyLeader)
        response.Result = NotifySetTimeResult::SetTimeResult::kNoPermission;
    else
    {
        auto& calendar = m_world.GetCalendarService();
        const int cHours = static_cast<int>(acMessage.Packet.Hours);
        const int cMinutes = static_cast<int>(acMessage.Packet.Minutes);
        response.Result = calendar.SetTime(cHours, cMinutes, calendar.GetTimeScale()) ?
            NotifySetTimeResult::SetTimeResult::kSuccess : NotifySetTimeResult::SetTimeResult::kInvalidTime;
    }

    acMessage.pPlayer->Send(response);
}

void CommandService::OnTeleportCommandRequest(const PacketEvent<TeleportCommandRequest>& acMessage) const noexcept
{
    Player* pTargetPlayer = nullptr;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer->GetUsername() == acMessage.Packet.TargetPlayer)
            pTargetPlayer = pPlayer;
    }

    TeleportCommandResponse response{};
    if (pTargetPlayer)
    {
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
    }

    acMessage.pPlayer->Send(response);
}
