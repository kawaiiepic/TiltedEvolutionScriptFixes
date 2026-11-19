#pragma once

#include <Events/PacketEvent.h>
#include <chrono>
#include <map>

struct PlayerLeaveEvent;
struct RequestPluginMessage;
struct World;

class PluginMessagingService
{
public:
    PluginMessagingService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;

private:
    void OnMessage(const PacketEvent<RequestPluginMessage>& acEvent) noexcept;
    void OnLeave(const PlayerLeaveEvent& acEvent) noexcept;

    struct Budget
    {
        std::chrono::steady_clock::time_point Updated;
        float Credits;
    };
    World& m_world;
    std::map<uint32_t, Budget> m_budgets;
    entt::scoped_connection m_messageConnection;
    entt::scoped_connection m_leaveConnection;
};
