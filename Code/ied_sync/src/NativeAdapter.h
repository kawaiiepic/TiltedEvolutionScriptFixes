#pragma once
#include "PCH.h"
#include "LocalIEDState.h"
#include <PluginMessagingAPI.h>

namespace IEDSyncTogether
{
    class NativeAdapter
    {
    public:
        static NativeAdapter& GetSingleton();
        void Start();
        void Reset();

    private:
        using Clock = std::chrono::steady_clock;
        struct Peer
        {
            std::uint32_t revision{};
            std::uint16_t count{}, next{};
            std::string partial;
            std::optional<LocalIEDState> state;
            Clock::time_point seen{};
            RE::FormID proxy{};
            bool dirty{};
        };
        static void Receive(std::uint32_t sender, const void* data, std::size_t size, void* context);
        void Tick();
        bool Discover();
        void UpdateSession(std::uint32_t id);
        void Accept(std::uint32_t sender, const void* data, std::size_t size);
        bool SendSnapshot(std::string_view payload);

        const SkyrimTogether::PluginMessagingInterface* _api{};
        std::uint32_t _local{}, _revision{};
        std::string _lastSent;
        Clock::time_point _nextSend{}, _heartbeat{}, _nextDiscovery{};
        std::unordered_map<std::uint32_t, Peer> _peers;
        std::atomic_bool _queued{false};
        std::jthread _timer;
    };
}
