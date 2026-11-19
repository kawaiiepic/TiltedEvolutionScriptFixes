#pragma once
#include <Messages/Trade.h>
#include <ModCompat/MagelightUI_API.h>
#include <atomic>
#include <chrono>

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
    void HandleHostEvent(MAGELIGHT_API::Event, MAGELIGHT_API::ViewId) noexcept;
    void CancelAndHide() noexcept;
    static void OnHostEvent(const MAGELIGHT_API::EventData*, void*) noexcept;
    static void OnCommand(MAGELIGHT_API::ViewId, const char*, void*) noexcept;
    World& m_world;
    const MAGELIGHT_API::MagelightApi4* m_api{};
    MAGELIGHT_API::ModId m_mod{};
    MAGELIGHT_API::ViewId m_view{};
    uint32_t m_dismissedSession{};
    bool m_ready{}, m_show{}, m_keyDown{}, m_focusRequested{};
    std::chrono::steady_clock::time_point m_openDeadline{};
    NotifyTrade m_state;
    std::atomic_bool m_queued{false};
    entt::scoped_connection m_notify, m_disconnect, m_update;
};
