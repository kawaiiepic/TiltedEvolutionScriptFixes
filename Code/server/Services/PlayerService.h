#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>
#include <chrono>
#include <unordered_map>
#include <map>
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
struct RequestFastTravelCatchUp;
struct RequestTravelResult;
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
    void HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) noexcept;
    void HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) noexcept;
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
        bool FastTravel{};
        GameId Cell{};
        GameId WorldSpace{};
        glm::vec3 Position{};
        std::chrono::steady_clock::time_point ExpiresAt{};
        TiltedPhoques::String NativeRoute;
        std::set<uint32_t> Invited;
        std::set<uint32_t> Accepted;
    };

    void OnFastTravelCatchUp(const PacketEvent<RequestFastTravelCatchUp>& acMessage) noexcept;
    void OfferCatchUp(const Player& acLeader, const glm::vec3& acPosition, const std::set<uint32_t>& acAlreadyTraveling,
                      bool aFastTravel) noexcept;
    void OfferNativeTravel(const Player& acLeader, std::string_view aRoute) noexcept;
    uint32_t NextTravelInvitationId() noexcept;
    void RetryTravel(const Player& acPlayer, uint32_t aPreviousId) noexcept;
    void OnTravelResult(const PacketEvent<RequestTravelResult>& acMessage) noexcept;
    void UpdateTravelInvitation(const UpdateEvent&) noexcept;
    void PublishTravelInvitation(const TravelInvitation& acInvitation, bool aClosed = false) const noexcept;
    void EndTravelInvitation(const TravelInvitation& acInvitation, std::string_view aMessage) const noexcept;
    bool UpdateTravelOffer(const TravelInvitation& acInvitation, std::chrono::steady_clock::time_point aNow) noexcept;
    bool IsTravelLeaderReady(const Player& acLeader, const glm::vec3& acPosition) const noexcept;
    bool CanJoinTravel(const Player& acPlayer, const TravelInvitation& acInvitation) const noexcept;

    World& m_world;
    std::map<uint32_t, TravelInvitation> m_travelInvitations;
    uint32_t m_nextTravelId{};
    struct CellDeparture
    {
        GameId Cell;
        std::chrono::steady_clock::time_point Time;
    };
    struct TravelDispatch
    {
        uint32_t Id{};
        uint32_t Leader{};
        bool FastTravel{};
        GameId Cell{};
        GameId WorldSpace{};
        glm::vec3 Position{};
        std::chrono::steady_clock::time_point Deadline;
    };
    std::unordered_map<uint32_t, TravelDispatch> m_travelDispatches;
    std::unordered_map<uint32_t, TravelDispatch> m_travelRetries;
    std::unordered_map<uint32_t, uint32_t> m_fastTravelSequences;
    std::unordered_map<uint32_t, CellDeparture> m_cellDepartures;
    std::unordered_map<uint32_t, CarriageBoarding> m_carriageBoardings;

    void GrantRecovery(const Player& acPlayer, entt::entity aCharacter, uint32_t aDownId, bool aRevived) const noexcept;

    entt::scoped_connection m_gridCellShiftConnection;
    entt::scoped_connection m_exteriorCellEnterConnection;
    entt::scoped_connection m_interiorCellEnterConnection;
    entt::scoped_connection m_playerRespawnConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_carriageTravelConnection;
    entt::scoped_connection m_travelUpdateConnection;
    entt::scoped_connection m_fastTravelCatchUpConnection;
    entt::scoped_connection m_travelResultConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_playerJoinConnection;
};
