#include <Services/PluginMessagingService.h>

#include <Components.h>
#include <Events/PlayerLeaveEvent.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <Messages/PluginMessage.h>
#include <World.h>

#include <algorithm>

PluginMessagingService::PluginMessagingService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_messageConnection = aDispatcher.sink<PacketEvent<RequestPluginMessage>>().connect<&PluginMessagingService::OnMessage>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&PluginMessagingService::OnLeave>(this);
}

void PluginMessagingService::OnLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    m_budgets.erase(acEvent.pPlayer->GetId());
}

void PluginMessagingService::OnMessage(const PacketEvent<RequestPluginMessage>& acEvent) noexcept
{
    const auto& request = acEvent.Packet;
    const auto& sender = *acEvent.pPlayer;
    const auto character = sender.GetCharacter();
    const auto& channel = request.Payload.Channel;
    if (!character || !sender.GetCellComponent().Cell ||
        !PluginMessagePayload::IsValidChannel({channel.data(), channel.size()}))
        return;

    constexpr float kBurst = 64.f;
    constexpr float kCreditsPerSecond = 32.f;
    const auto now = std::chrono::steady_clock::now();
    auto& budget = m_budgets.try_emplace(sender.GetId(), Budget{now, kBurst}).first->second;
    const float elapsed = std::chrono::duration<float>(now - budget.Updated).count();
    budget.Updated = now;
    budget.Credits = std::min(kBurst, budget.Credits + elapsed * kCreditsPerSecond);
    const float cost = 1.f + static_cast<float>(request.Payload.Bytes.size()) / 512.f;
    if (budget.Credits < cost)
        return;
    budget.Credits -= cost;

    NotifyPluginMessage notify{};
    notify.Sender = sender.GetId();
    notify.Payload = request.Payload;
    if (request.Target == PluginMessagePayload::Nearby)
    {
        GameServer::Get()->SendToPlayersInRange(notify, *character, &sender);
        return;
    }

    if (request.Target)
    {
        const auto* pTarget = m_world.GetPlayerManager().GetById(request.Target);
        if (pTarget && pTarget != &sender && pTarget->GetCharacter() && pTarget->GetCellComponent().Cell)
            pTarget->Send(notify);
        return;
    }

    for (const auto* pPlayer : m_world.GetPlayerManager())
        if (pPlayer != &sender && pPlayer->GetCharacter() && pPlayer->GetCellComponent().Cell)
            pPlayer->Send(notify);
}
