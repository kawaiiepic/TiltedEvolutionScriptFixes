#pragma once

#include "PCH.h"
#include "LocalIEDState.h"

namespace IEDSyncTogether
{
    class RemoteIEDRenderer
    {
    public:
        static RemoteIEDRenderer& GetSingleton();

        void Start();

        bool Apply(
            std::uint32_t connectionID,
            RE::FormID proxyFormID,
            std::string_view displayName,
            const LocalIEDState& state,
            bool force = false);

        void ClearProxy(RE::FormID proxyFormID);
        void Reset();

    private:
        struct TrackedRemoteObject
        {
            std::string itemName;
            std::string kind;
            std::optional<std::size_t> slot;
            RE::FormID remoteFormID{ 0 };
            std::string expectedAttachment;
            CapturedIEDObject object;
            bool acquired{ false };
            std::uint32_t corrections{ 0 };
        };

        struct TrackedProxyState
        {
            std::uint32_t connectionID{ 0 };
            std::string displayName;
            std::vector<TrackedRemoteObject> objects;
        };

        RemoteIEDRenderer() = default;

        void WatchdogLoop(std::stop_token stopToken);
        void ScheduleWatchdogTick();
        void WatchdogTick();

        std::unordered_map<RE::FormID, std::string> _appliedSignatures;
        std::unordered_map<RE::FormID, TrackedProxyState> _trackedProxies;
        std::jthread _watchdogThread;
        std::atomic_bool _watchdogTaskQueued{ false };
    };
}
