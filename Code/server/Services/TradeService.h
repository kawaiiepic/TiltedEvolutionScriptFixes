#pragma once
#include <Events/PacketEvent.h>
#include <Messages/Trade.h>
#include <Structs/Inventory.h>
#include <array>
#include <chrono>
#include <map>

struct World;
struct Player;
struct PlayerLeaveEvent;
struct UpdateEvent;

class TradeService
{
public:
    TradeService(World&, entt::dispatcher&) noexcept;
private:
    using Clock = std::chrono::steady_clock;
    struct Side
    {
        uint32_t PlayerId{};
        bool Confirmed{};
        TiltedPhoques::Vector<Inventory::Entry> Catalog;
        TiltedPhoques::Vector<uint32_t> Offered;
    };
    struct Session
    {
        uint32_t Id{}, Revision{1};
        bool Accepted{};
        std::array<Side, 2> Sides;
        Clock::time_point Expires;
    };
    void OnRequest(const PacketEvent<RequestTrade>&) noexcept;
    void OnLeave(const PlayerLeaveEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    bool Nearby(const Player&, const Player&) const noexcept;
    bool Capture(Side&, Player&) noexcept;
    bool Commit(Session&, Player&, Player&) noexcept;
    void Publish(const Session&, uint8_t phase) noexcept;
    void Close(uint32_t, uint8_t phase) noexcept;
    void Error(Player&) noexcept;
    uint32_t Find(uint32_t player) const noexcept;
    World& m_world;
    uint32_t m_nextId{};
    std::map<uint32_t, Session> m_sessions;
    Clock::time_point m_nextTick{};
    entt::scoped_connection m_request, m_leave, m_update;
};
