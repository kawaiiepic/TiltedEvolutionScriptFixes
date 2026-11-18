#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>
#include <chrono>
#include <unordered_map>

struct World;
struct ShiftGridCellRequest;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct PlayerRespawnRequest;
struct PlayerLevelRequest;
struct PlayerLeaveEvent;
struct RequestCarriageTravel;

/**
 * @brief Handles player specific actions that might change the information needed by other clients about that player.
 */
struct PlayerService
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);

protected:
    void HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept;
    void HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept;
    void HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept;
    void OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept;
    void OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept;
    void OnCarriageTravel(const PacketEvent<RequestCarriageTravel>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

private:
    struct CarriageBoarding
    {
        GameId Seat;
        GameId Cell;
        glm::vec3 Position;
        uint32_t Token;
        std::chrono::steady_clock::time_point ExpiresAt;
    };

    World& m_world;
    std::unordered_map<uint32_t, CarriageBoarding> m_carriageBoardings;

    entt::scoped_connection m_gridCellShiftConnection;
    entt::scoped_connection m_exteriorCellEnterConnection;
    entt::scoped_connection m_interiorCellEnterConnection;
    entt::scoped_connection m_playerRespawnConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_carriageTravelConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
