#pragma once

#include <Structs/GameId.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct CellChangeEvent;
struct DisconnectedEvent;
struct NotifyWeatherChange;

struct WeatherService
{
    WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher);
    TP_NOCOPYMOVE(WeatherService);

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnCellChange(const CellChangeEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept;
    void ReleaseOverride() noexcept;

    World& m_world;
    TransportService& m_transport;
    GameId m_cell{};
    GameId m_target{};
    uint32_t m_owner{};
    uint32_t m_lastReported{};
    bool m_appliedOverride{};
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_cellConnection;
    entt::scoped_connection m_disconnectConnection;
    entt::scoped_connection m_weatherChangeConnection;
};
