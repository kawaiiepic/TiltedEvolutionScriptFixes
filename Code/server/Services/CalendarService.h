#pragma once

#include <Events/PacketEvent.h>
#include <DateTime.h>
#include <Structs/GameId.h>
#include <chrono>
#include <optional>
#include <set>
#include <string_view>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerJoinEvent;

/**
 * @brief Manages time and date of the world.
 */
class CalendarService
{
public:
    CalendarService(World& aWorld, entt::dispatcher& aDispatcher);

    // we use these types for SOL
    // this is done this way because SOL
    // provides direct support for these
    using TTime = std::pair<int, int>;
    using TDate = std::tuple<int, int, int>;

    bool HandleRestCommand(const Player& acPlayer, std::string_view aCommand) noexcept;
    bool SetTime(int aHour, int aMinutes, float aScale) noexcept;
    bool SetDate(int aDay, int aMonth, float aYear) noexcept;

    // returns hours, minutes
    TTime GetTime() const noexcept;
    static TTime GetRealTime() noexcept;

    // returns dd/mm/yy
    TDate GetDate() const noexcept;

    float GetTimeScale() const noexcept { return m_dateTime.m_timeModel.TimeScale; }
    bool SetTimeScale(float aScale) noexcept;

private:
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent&) noexcept;
    void SendTimeResync(uint8_t aRestHours = 0) noexcept;
    std::set<uint32_t> GetRestVoters() const;
    void UpdateRestVote() noexcept;

    struct RestVote
    {
        uint32_t Id{};
        uint32_t Organizer{};
        uint32_t Hours{};
        bool Sleep{};
        std::set<uint32_t> Voters;
        std::set<uint32_t> Approved;
        std::chrono::steady_clock::time_point Deadline;
    };
    std::optional<RestVote> m_restVote;
    uint32_t m_nextRestVote{};

    DateTime m_dateTime;
    uint64_t m_lastTick = 0;
    bool m_timeSetFromFirstPlayer = false;

    World& m_world;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
