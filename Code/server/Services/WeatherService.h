#pragma once

#include <Events/PacketEvent.h>
#include <Messages/NotifyWeatherChange.h>
#include <chrono>
#include <unordered_map>

struct World;
struct UpdateEvent;
struct RequestWeatherChange;

struct WeatherService
{
    WeatherService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    TP_NOCOPYMOVE(WeatherService);

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnWeatherChange(const PacketEvent<RequestWeatherChange>& acMessage) noexcept;

    World& m_world;
    std::unordered_map<GameId, NotifyWeatherChange> m_cells;
    std::unordered_map<uint32_t, NotifyWeatherChange> m_lastSent;
    std::chrono::steady_clock::time_point m_nextUpdate{};
    entt::scoped_connection m_weatherChangeConnection;
    entt::scoped_connection m_updateConnection;
};
