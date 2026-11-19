#pragma once
#include <PluginMessagingAPI.h>
#include <RE/Skyrim.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <thread>

namespace AnimSyncTogether
{
    class NativeClient final
    {
    public:
        static NativeClient* GetSingleton();
        bool Initialize();
        void Reset();
        bool IsAvailable() const noexcept;
        std::optional<std::uint32_t> FindConnectionID(RE::FormID formID) const;
        bool SendAnimationEvent(std::string_view tag, std::string_view payload,
            std::int32_t animationType, bool hasAnimationType, std::int32_t offsetType, bool hasOffsetType);
        bool SendGraphVariableBool(std::string_view name, bool value);
        bool SendGraphVariableInt(std::string_view name, std::int32_t value);
        bool SendGraphVariableFloat(std::string_view name, float value);
    private:
        using Clock = std::chrono::steady_clock;
        struct Peer { RE::FormID form{}; Clock::time_point seen{}; };
        static void Receive(std::uint32_t sender, const void* data, std::size_t size, void* context);
        bool Send(const void* data, std::size_t size);
        void Tick();
        RE::FormID Resolve(std::uint32_t sender);
        void ApplyAnimationMessage(std::uint32_t senderConnectionID, const std::string& tag,
            const std::string& payload, std::uint32_t stateFlags, std::int32_t gpmaAnimationType,
            std::int32_t gpmaOffsetType);
        void ApplyGraphVariableMessage(std::uint32_t senderConnectionID, const std::string& name,
            std::uint32_t variableType, bool boolValue, std::int32_t intValue, float floatValue);
        const SkyrimTogether::PluginMessagingInterface* messaging_{};
        std::uint32_t local_{};
        std::unordered_map<std::uint32_t, Peer> peers_;
        std::unordered_map<RE::FormID, std::uint32_t> formIDToConnection_;
        Clock::time_point nextRefresh_{};
        std::atomic_bool queued_{false};
        std::jthread timer_;
    };
}
