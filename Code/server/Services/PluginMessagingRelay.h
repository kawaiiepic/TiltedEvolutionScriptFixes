#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string_view>

struct Player;
struct World;

class PluginMessagingRelay
{
public:
    static bool IsEnabled() noexcept;
    void Relay(World& aWorld, const Player& acSender, std::string_view aMessage) noexcept;
    void Forget(uint32_t aPlayerId) noexcept;

private:
    struct Budget
    {
        std::chrono::steady_clock::time_point LastUpdate{};
        float Credits{};
    };
    bool ConsumeBudget(uint32_t aPlayerId, size_t aSize) noexcept;
    std::map<uint32_t, Budget> m_budgets;
};
