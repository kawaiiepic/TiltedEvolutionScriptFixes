#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>
#include <chrono>
#include <unordered_map>
#include <optional>
#include <set>
#include <string_view>

struct World;
struct ShiftGridCellRequest;
struct EnterInteriorCellRequest;
struct EnterExteriorCellRequest;
struct PlayerRespawnRequest;
struct Player;
struct PlayerJoinEvent;
struct PlayerLevelRequest;
struct PlayerLeaveEvent;
struct RequestCarriageTravel;
struct UpdateEvent;

/**
 * @brief Handles player specific actions that might change the information needed by other clients about that player.
 */
struct PlayerService
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);
    bool HandleTravelCommand(const Player& acPlayer, std::string_view aCommand) noexcept;

protected:
    void HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept;
    void HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept;
    void HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept;
    void OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept;
    void OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept;
    void OnCarriageTravel(const PacketEvent<RequestCarriageTravel>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) const noexcept;

private:
    struct CarriageBoarding
    {
        GameId Seat;
        GameId Cell;
        glm::vec3 Position;
        uint32_t Token;
        std::chrono::steady_clock::time_point ExpiresAt;
    };

    struct TravelInvitation
    {
        uint32_t Id{};
        uint32_t Leader{};
        GameId Cell{};
        GameId WorldSpace{};
        glm::vec3 Position{};
        std::chrono::steady_clock::time_point ExpiresAt{};
        std::set<uint32_t> Invited;
        std::set<uint32_t> Accepted;
    };

    void UpdateTravelInvitation(const UpdateEvent&) noexcept;
    void EndTravelInvitation(std::string_view aMessage) noexcept;
    bool CanJoinTravel(const Player& acPlayer, const TravelInvitation& acInvitation) const noexcept;

    World& m_world;
    std::optional<TravelInvitation> m_travelInvitation;
    uint32_t m_nextTravelId{};
    std::unordered_map<uint32_t, CarriageBoarding> m_carriageBoardings;

    void GrantRecovery(const Player& acPlayer, entt::entity aCharacter, uint32_t aDownId, bool aRevived) const noexcept;

    entt::scoped_connection m_gridCellShiftConnection;
    entt::scoped_connection m_exteriorCellEnterConnection;
    entt::scoped_connection m_interiorCellEnterConnection;
    entt::scoped_connection m_playerRespawnConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_carriageTravelConnection;
    entt::scoped_connection m_travelUpdateConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_playerJoinConnection;
};
