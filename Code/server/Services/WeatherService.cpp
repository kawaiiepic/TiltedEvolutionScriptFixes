#include <Services/WeatherService.h>

#include <Components.h>
#include <World.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestWeatherChange.h>

WeatherService::WeatherService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_weatherChangeConnection = aDispatcher.sink<PacketEvent<RequestWeatherChange>>().connect<&WeatherService::OnWeatherChange>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WeatherService::OnUpdate>(this);
}

void WeatherService::OnUpdate(const UpdateEvent&) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextUpdate)
        return;
    m_nextUpdate = now + std::chrono::milliseconds(250);

    // Weather regions are not exposed by the engine wrapper. An exterior
    // cell is a conservative boundary that also works in modded worldspaces.
    std::unordered_map<GameId, NotifyWeatherChange> cells;
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        const auto& position = pPlayer->GetCellComponent();
        if (!pPlayer->GetCharacter() || !position.Cell || position.IsInInteriorCell())
            continue;

        auto& state = cells[position.Cell];
        if (state.Owner)
            continue;
        state.Cell = position.Cell;
        state.Owner = pPlayer->GetId();

        if (const auto previous = m_cells.find(position.Cell); previous != m_cells.end())
        {
            state.Id = previous->second.Id;
            const auto* pOwner = m_world.GetPlayerManager().GetById(previous->second.Owner);
            if (pOwner && pOwner->GetCharacter() && pOwner->GetCellComponent().Cell == position.Cell &&
                pOwner->GetCellComponent().WorldSpaceId == position.WorldSpaceId)
                state.Owner = previous->second.Owner;
        }
    }
    m_cells = std::move(cells);

    std::unordered_map<uint32_t, NotifyWeatherChange> sent;
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        const auto& position = pPlayer->GetCellComponent();
        if (!pPlayer->GetCharacter() || position.IsInInteriorCell())
            continue;
        const auto cell = m_cells.find(position.Cell);
        if (cell == m_cells.end())
            continue;

        auto state = cell->second;
        if (state.Id && !pPlayer->HasModId(state.Id.ModId))
            state.Id = {};
        const auto previous = m_lastSent.find(pPlayer->GetId());
        if (previous == m_lastSent.end() || !(previous->second == state))
            pPlayer->Send(state);
        sent.emplace(pPlayer->GetId(), state);
    }
    m_lastSent = std::move(sent);
}

void WeatherService::OnWeatherChange(const PacketEvent<RequestWeatherChange>& acMessage) noexcept
{
    const auto& request = acMessage.Packet;
    const auto& position = acMessage.pPlayer->GetCellComponent();
    if (!request.Cell || position.Cell != request.Cell || position.IsInInteriorCell() ||
        !m_world.ctx().at<ModsComponent>().FindFormKey(request.Id, *acMessage.pPlayer))
        return;

    const auto cell = m_cells.find(request.Cell);
    if (cell != m_cells.end() && cell->second.Owner == acMessage.pPlayer->GetId())
        cell->second.Id = request.Id;
}
