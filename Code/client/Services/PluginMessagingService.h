#pragma once

#include <PluginMessagingAPI.h>

struct NotifyPluginMessage;

class PluginMessagingService
{
public:
    explicit PluginMessagingService(entt::dispatcher& aDispatcher) noexcept;
    static const SkyrimTogether::PluginMessagingInterface& GetInterface() noexcept;

private:
    void OnMessage(const NotifyPluginMessage& acMessage) noexcept;
    entt::scoped_connection m_messageConnection;
};
