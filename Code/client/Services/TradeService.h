#pragma once
#include <Messages/Trade.h>
#include <ModCompat/PrismaUI_API.h>
#include <atomic>

struct World;
struct UpdateEvent;
struct DisconnectedEvent;

class TradeService
{
public:
    TradeService(World&, entt::dispatcher&) noexcept;
    void InviteTarget() noexcept;
private:
    bool EnsureView() noexcept;
    void Present() noexcept;
    void Hide() noexcept;
    void OnNotify(const NotifyTrade&) noexcept;
    void OnDisconnect(const DisconnectedEvent&) noexcept;
    void OnUpdate(const UpdateEvent&) noexcept;
    void Tick() noexcept;
    void Command(const std::string&) noexcept;
    static void DomReady(PrismaView) noexcept;
    static void OnCommand(const char*) noexcept;
    World& m_world;
    PRISMA_UI_API::IVPrismaUI1* m_api{};
    PrismaView m_view{};
    bool m_ready{}, m_show{}, m_keyDown{};
    NotifyTrade m_state;
    std::atomic_bool m_queued{false};
    entt::scoped_connection m_notify, m_disconnect, m_update;
};
