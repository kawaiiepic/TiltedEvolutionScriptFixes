#include <TiltedOnlinePCH.h>

#include <PluginMessagingAPI.h>
#include <Services/PluginMessagingService.h>
#include <Messages/PluginMessage.h>
#include <World.h>
#include <Components.h>
#include <Actor.h>

#include <map>
#include <mutex>
#include <string>

namespace
{
using namespace SkyrimTogether;
using Result = PluginMessageResult;

static_assert(MaxChannelLength == PluginMessagePayload::MaxChannelLength);
static_assert(MaxPayloadSize == PluginMessagePayload::MaxPayloadSize);
static_assert(Nearby == PluginMessagePayload::Nearby);

struct Listener
{
    PluginMessageCallback Callback{};
    void* pContext{};
};

std::mutex s_listenersMutex;
std::map<std::string, Listener, std::less<>> s_listeners;

Result Register(const char* apChannel, PluginMessageCallback aCallback, void* apContext)
{
    if (!apChannel || !aCallback || !PluginMessagePayload::IsValidChannel(apChannel))
        return Result::InvalidArgument;

    std::lock_guard lock(s_listenersMutex);
    if (s_listeners.contains(apChannel))
        return Result::ChannelInUse;
    if (s_listeners.size() >= 64)
        return Result::LimitReached;
    s_listeners.emplace(apChannel, Listener{aCallback, apContext});
    return Result::Ok;
}

Result Unregister(const char* apChannel, PluginMessageCallback aCallback)
{
    if (!apChannel || !aCallback || !PluginMessagePayload::IsValidChannel(apChannel))
        return Result::InvalidArgument;

    std::lock_guard lock(s_listenersMutex);
    const auto it = s_listeners.find(apChannel);
    if (it == s_listeners.end() || it->second.Callback != aCallback)
        return Result::ChannelNotRegistered;
    s_listeners.erase(it);
    return Result::Ok;
}

Result Send(const char* apChannel, uint32_t aTarget, const void* apData, size_t aSize)
{
    if (!apChannel || !PluginMessagePayload::IsValidChannel(apChannel) || aSize > MaxPayloadSize || (aSize && !apData))
        return Result::InvalidArgument;
    if (!entt::locator<World>::has_value())
        return Result::NotConnected;

    auto& world = World::Get();
    if (!world.IsGameThread())
        return Result::WrongThread;
    if (!world.GetTransport().IsOnline())
        return Result::NotConnected;

    RequestPluginMessage request{};
    request.Target = aTarget;
    request.Payload.Channel = apChannel;
    if (aSize)
    {
        const auto* pBytes = static_cast<const uint8_t*>(apData);
        request.Payload.Bytes.assign(pBytes, pBytes + aSize);
    }
    return world.GetTransport().Send(request) ? Result::Ok : Result::SendFailed;
}

uint32_t LocalPlayerId()
{
    if (!entt::locator<World>::has_value())
        return 0;
    auto& world = World::Get();
    return world.IsGameThread() && world.GetTransport().IsOnline() ? world.GetTransport().GetLocalPlayerId() : 0;
}

uint32_t ResolvePlayerFormId(uint32_t aPlayerId)
{
    const uint32_t local = LocalPlayerId();
    if (!local || !aPlayerId)
        return 0;
    if (aPlayerId == local)
        return 0x14;

    auto& world = World::Get();
    const auto view = world.view<PlayerComponent, RemoteComponent, FormIdComponent>(entt::exclude<WaitingFor3D>);
    for (const auto entity : view)
    {
        if (view.get<PlayerComponent>(entity).Id != aPlayerId)
            continue;
        const uint32_t formId = view.get<FormIdComponent>(entity).Id;
        if (Cast<Actor>(TESForm::GetById(formId)))
            return formId;
    }
    return 0;
}

const PluginMessagingInterface kInterface{PluginMessagingVersion, &Register, &Unregister, &Send, &LocalPlayerId, &ResolvePlayerFormId};
}

extern "C" __declspec(dllexport) SkyrimTogether::PluginMessageResult
ST_QueryPluginMessaging(uint32_t aVersion, const SkyrimTogether::PluginMessagingInterface** appInterface) noexcept
{
    if (!appInterface)
        return Result::InvalidArgument;
    *appInterface = nullptr;
    if (aVersion != SkyrimTogether::PluginMessagingVersion)
        return Result::UnsupportedVersion;
    *appInterface = &PluginMessagingService::GetInterface();
    return Result::Ok;
}

const SkyrimTogether::PluginMessagingInterface& PluginMessagingService::GetInterface() noexcept
{
    return kInterface;
}

PluginMessagingService::PluginMessagingService(entt::dispatcher& aDispatcher) noexcept
{
    m_messageConnection = aDispatcher.sink<NotifyPluginMessage>().connect<&PluginMessagingService::OnMessage>(this);
}

void PluginMessagingService::OnMessage(const NotifyPluginMessage& acMessage) noexcept
{
    auto& world = World::Get();
    if (!world.IsGameThread())
    {
        const uint32_t playerId = world.GetTransport().GetLocalPlayerId();
        world.QueueCheckpointTask([this, message = acMessage, playerId]()
        {
            if (LocalPlayerId() == playerId && playerId)
                OnMessage(message);
        });
        return;
    }

    const auto& payload = acMessage.Payload;
    const std::string_view channel(payload.Channel.data(), payload.Channel.size());
    if (!acMessage.Sender || !PluginMessagePayload::IsValidChannel(channel))
        return;

    Listener listener;
    {
        std::lock_guard lock(s_listenersMutex);
        const auto it = s_listeners.find(channel);
        if (it == s_listeners.end())
            return;
        listener = it->second;
    }
    listener.Callback(acMessage.Sender, payload.Bytes.data(), payload.Bytes.size(), listener.pContext);
}
