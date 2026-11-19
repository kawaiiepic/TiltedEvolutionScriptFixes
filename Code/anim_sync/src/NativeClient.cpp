#include "AnimSyncTogether/NativeClient.h"

#include "AnimSyncTogether/AnimationClipProbe.h"
#include "AnimSyncTogether/AnimationProbe.h"
#include "AnimSyncTogether/OARClient.h"
#include "AnimSyncTogether/SyncRules.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <Windows.h>
#include <cmath>
#include <chrono>
#include "AnimSyncTogether/GraphVariableSync.h"

namespace AnimSyncTogether
{
    namespace
    {
        constexpr char kAnimationChannel[] = "skyrimtogether.animsync.anim.v1";
        constexpr std::uint32_t kSyncPacketVersion = 3;
        constexpr std::uint32_t kStateHasGPMAAnimationType = 1u << 0;
        constexpr std::uint32_t kStateHasGPMAOffsetType = 1u << 1;

        enum class SyncPacketKind : std::uint32_t
        {
            kAnimationEvent = 1,
            kGraphVariable = 2
        };

        struct SyncPacket
        {
            std::uint32_t version{ kSyncPacketVersion };
            std::uint32_t kind{ static_cast<std::uint32_t>(SyncPacketKind::kAnimationEvent) };
            std::uint32_t stateFlags{ 0 };
            std::uint32_t graphVariableType{ 0 };
            std::int32_t gpmaAnimationType{ 0 };
            std::int32_t gpmaOffsetType{ 0 };
            std::int32_t graphVariableInt{ 0 };
            float graphVariableFloat{ 0.0F };
            std::uint32_t graphVariableBool{ 0 };
            std::array<char, 64> tag{};
            std::array<char, 128> payload{};
        };

        void CopyField(std::span<char> destination, std::string_view source)
        {
            if (destination.empty()) {
                return;
            }

            const auto count = (std::min)(source.size(), destination.size() - 1);
            std::memcpy(destination.data(), source.data(), count);
            destination[count] = '\0';
        }

        static_assert(sizeof(SyncPacket) <= SkyrimTogether::MaxPayloadSize);
        constexpr size_t kMaxPeers = 64;
        constexpr std::uint32_t kHeartbeatKind = 3;
        bool ValidField(std::string_view value, size_t max)
        {
            return !value.empty() && value.size() < max && value.find('\0') == value.npos;
        }
    }

    NativeClient* NativeClient::GetSingleton()
    {
        static NativeClient singleton;
        return std::addressof(singleton);
    }

    bool NativeClient::Initialize()
    {
        OARClient::GetSingleton()->Initialize();
        SyncRules::GetSingleton()->Initialize();
        if (!timer_.joinable()) {
            timer_ = std::jthread([this](std::stop_token token) {
                while (!token.stop_requested()) {
                    if (auto* tasks = SKSE::GetTaskInterface(); tasks && !queued_.exchange(true)) {
                        tasks->AddTask([this]() { Tick(); queued_.store(false); });
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            });
        }
        if (messaging_) return true;
        auto module = GetModuleHandleW(L"SkyrimTogether.exe");
        if (!module) module = GetModuleHandleW(nullptr);
        const auto query = reinterpret_cast<SkyrimTogether::QueryPluginMessaging>(
            GetProcAddress(module, SkyrimTogether::PluginMessagingExport));
        const SkyrimTogether::PluginMessagingInterface* api{};
        using Result = SkyrimTogether::PluginMessageResult;
        if (!query || query(SkyrimTogether::PluginMessagingVersion, &api) != Result::Ok ||
            !api || !api->Register || !api->Send || !api->ResolvePlayerFormId || !api->LocalPlayerId ||
            api->Register(kAnimationChannel, &Receive, this) != Result::Ok) return false;
        messaging_ = api;
        SKSE::log::info("AnimSync native messaging registered");
        return true;
    }

    bool NativeClient::IsAvailable() const noexcept { return messaging_ != nullptr; }

    void NativeClient::Reset()
    {
        for (const auto& [form, id] : formIDToConnection_) {
            (void)id;
            AnimationProbe::GetSingleton()->DetachActorByFormID(form, "native session reset");
        }
        formIDToConnection_.clear();
        peers_.clear();
        local_ = 0;
        nextRefresh_ = {};
    }

    std::optional<std::uint32_t> NativeClient::FindConnectionID(RE::FormID formID) const
    {
        const auto it = formIDToConnection_.find(formID);
        if (it == formIDToConnection_.end()) return std::nullopt;
        return it->second;
    }

    RE::FormID NativeClient::Resolve(std::uint32_t sender)
    {
        if (!messaging_ || !sender || sender == local_) return 0;
        const auto form = messaging_->ResolvePlayerFormId(sender);
        auto it = peers_.find(sender);
        if (it == peers_.end()) return 0;
        auto& peer = it->second;
        if (peer.form != form) {
            if (peer.form) {
                AnimationProbe::GetSingleton()->DetachActorByFormID(peer.form, "native proxy changed");
                formIDToConnection_.erase(peer.form);
            }
            peer.form = form;
        }
        if (form && form != 0x14) {
            formIDToConnection_[form] = sender;
            AnimationProbe::GetSingleton()->AttachActorByFormID(form, "native proxy");
            return form;
        }
        return 0;
    }

    void NativeClient::Tick()
    {
        if (!messaging_ && !Initialize()) return;
        const auto id = messaging_->LocalPlayerId();
        if (local_ != id) {
            Reset();
            local_ = id;
            GraphVariableSync::GetSingleton()->Reset();
        }
        if (!local_) return;
        const auto now = Clock::now();
        if (now >= nextRefresh_) {
            nextRefresh_ = now + std::chrono::seconds(10);
            SyncPacket heartbeat{};
            heartbeat.kind = kHeartbeatKind;
            Send(&heartbeat, sizeof(heartbeat));
            GraphVariableSync::GetSingleton()->Reset();
            GraphVariableSync::GetSingleton()->CaptureAndSend(RE::PlayerCharacter::GetSingleton());
        }
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (now - it->second.seen > std::chrono::seconds(40)) {
                if (it->second.form) {
                    AnimationProbe::GetSingleton()->DetachActorByFormID(it->second.form, "native peer expired");
                    formIDToConnection_.erase(it->second.form);
                }
                it = peers_.erase(it);
            } else {
                Resolve(it->first);
                ++it;
            }
        }
    }

    bool NativeClient::Send(const void* data, std::size_t size)
    {
        if (!messaging_ || size != sizeof(SyncPacket)) return false;
        return messaging_->Send(kAnimationChannel, SkyrimTogether::Broadcast, data, size) ==
            SkyrimTogether::PluginMessageResult::Ok;
    }

    void NativeClient::Receive(std::uint32_t sender, const void* data, std::size_t size, void* context)
    {
        if (!context || !data || size != sizeof(SyncPacket) || !sender) return;
        auto& self = *static_cast<NativeClient*>(context);
        if (!self.messaging_) return;
        const auto local = self.messaging_->LocalPlayerId();
        if (!local || local == sender) return;
        if (self.local_ != local) {
            self.Reset();
            self.local_ = local;
            GraphVariableSync::GetSingleton()->Reset();
        }
        SyncPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        if (packet.version != kSyncPacketVersion || (packet.stateFlags & ~3u)) return;
        if (packet.kind != kHeartbeatKind && packet.kind != 1 && packet.kind != 2) return;
        if (!self.peers_.contains(sender) && self.peers_.size() >= kMaxPeers) return;
        self.peers_[sender].seen = Clock::now();
        if (packet.kind == kHeartbeatKind) { self.Resolve(sender); return; }
        if (packet.tag.back() || packet.payload.back()) return;
        const std::string tag(packet.tag.data());
        if (tag.empty()) return;
        if (packet.kind == 1) {
            self.ApplyAnimationMessage(sender, tag, std::string(packet.payload.data()),
                packet.stateFlags, packet.gpmaAnimationType, packet.gpmaOffsetType);
        } else if (packet.graphVariableBool <= 1 && std::isfinite(packet.graphVariableFloat)) {
            self.ApplyGraphVariableMessage(sender, tag, packet.graphVariableType,
                packet.graphVariableBool != 0, packet.graphVariableInt, packet.graphVariableFloat);
        }
    }

    bool NativeClient::SendAnimationEvent(std::string_view tag, std::string_view payload,
        std::int32_t animationType, bool hasAnimationType, std::int32_t offsetType, bool hasOffsetType)
    {
        if (!ValidField(tag, 64) || payload.size() >= 128) return false;
        SyncPacket packet{};
        if (hasAnimationType) { packet.stateFlags |= kStateHasGPMAAnimationType; packet.gpmaAnimationType = animationType; }
        if (hasOffsetType) { packet.stateFlags |= kStateHasGPMAOffsetType; packet.gpmaOffsetType = offsetType; }
        CopyField(packet.tag, tag);
        CopyField(packet.payload, payload);
        return Send(&packet, sizeof(packet));
    }

    bool NativeClient::SendGraphVariableBool(std::string_view name, bool value)
    {
        if (!ValidField(name, 64)) return false;
        SyncPacket packet{};
        packet.kind = 2;
        packet.graphVariableType = static_cast<std::uint32_t>(GraphVariableType::kBool);
        packet.graphVariableBool = value ? 1u : 0u;
        CopyField(packet.tag, name);
        return Send(&packet, sizeof(packet));
    }

    bool NativeClient::SendGraphVariableInt(std::string_view name, std::int32_t value)
    {
        if (!ValidField(name, 64)) return false;
        SyncPacket packet{};
        packet.kind = 2;
        packet.graphVariableType = static_cast<std::uint32_t>(GraphVariableType::kInt);
        packet.graphVariableInt = value;
        CopyField(packet.tag, name);
        return Send(&packet, sizeof(packet));
    }

    bool NativeClient::SendGraphVariableFloat(std::string_view name, float value)
    {
        if (!ValidField(name, 64) || !std::isfinite(value)) return false;
        SyncPacket packet{};
        packet.kind = 2;
        packet.graphVariableType = static_cast<std::uint32_t>(GraphVariableType::kFloat);
        packet.graphVariableFloat = value;
        CopyField(packet.tag, name);
        return Send(&packet, sizeof(packet));
    }

    void NativeClient::ApplyAnimationMessage(
        std::uint32_t senderConnectionID,
        const std::string& tag,
        const std::string& payload,
        std::uint32_t stateFlags,
        std::int32_t gpmaAnimationType,
        std::int32_t gpmaOffsetType)
    {
        (void)payload;


        const auto formID = Resolve(senderConnectionID);
        if (!formID) return;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
        if (!actor) {
            SKSE::log::info(
                "AnimRxInput sender={} proxy={:08X} actor='<missing>' event='{}' replay=false",
                senderConnectionID,
                formID,
                tag);
            return;
        }

        if (!SyncRules::GetSingleton()->ShouldSyncEvent(tag)) {
            SKSE::log::info(
                "AnimRxInput sender={} proxy={:08X} actor='{}' event='{}' replay=false reason='not in sync rules'",
                senderConnectionID,
                formID,
                actor->GetName(),
                tag);
            return;
        }

        const RE::BSFixedString animationTypeVariable{ "iGPMAAnimationType" };
        const RE::BSFixedString offsetTypeVariable{ "iGPMAOffsetType" };

        if (tag == "OffsetGPMA") {
            bool animationTypeApplied = false;
            bool offsetTypeApplied = false;

            if ((stateFlags & kStateHasGPMAAnimationType) != 0) {
                animationTypeApplied = actor->SetGraphVariableInt(animationTypeVariable, gpmaAnimationType);
            }
            if ((stateFlags & kStateHasGPMAOffsetType) != 0) {
                offsetTypeApplied = actor->SetGraphVariableInt(offsetTypeVariable, gpmaOffsetType);
            }

            SKSE::log::info(
                "GPMAStateApply actor={:08X} event='OffsetGPMA' animationTypePresent={} animationType={} animationTypeApplied={} offsetTypePresent={} offsetType={} offsetTypeApplied={}",
                formID,
                (stateFlags & kStateHasGPMAAnimationType) != 0,
                gpmaAnimationType,
                animationTypeApplied,
                (stateFlags & kStateHasGPMAOffsetType) != 0,
                gpmaOffsetType,
                offsetTypeApplied);

            AnimationClipProbe::ArmActor(formID, "remote OffsetGPMA with GPMA state");
        }

        // Graph-variable packets are delivered on the same reliable ordered
        // channel before the matching custom event. Refresh OAR immediately before
        // replay so its replacement conditions see the synchronized proxy state.
        OARClient::GetSingleton()->ClearConditionStateData(actor);

        const RE::BSFixedString inputEvent{ tag.c_str() };
        const bool replayed = actor->NotifyAnimationGraph(inputEvent);

        SKSE::log::info(
            "AnimRxInput sender={} proxy={:08X} actor='{}' event='{}' replay=true result={} stateFlags=0x{:X} animationType={} offsetType={}",
            senderConnectionID,
            formID,
            actor->GetName(),
            tag,
            replayed,
            stateFlags,
            gpmaAnimationType,
            gpmaOffsetType);

        if (tag == "OffsetGPMAStop") {
            const bool reset = actor->SetGraphVariableInt(animationTypeVariable, 0);
            SKSE::log::info(
                "GPMAStateReset actor={:08X} variable='iGPMAAnimationType' value=0 result={} source='network stop'",
                formID,
                reset);
        }
    }

    void NativeClient::ApplyGraphVariableMessage(
        std::uint32_t senderConnectionID,
        const std::string& name,
        std::uint32_t variableType,
        bool boolValue,
        std::int32_t intValue,
        float floatValue)
    {

        const auto type = static_cast<GraphVariableType>(variableType);
        if (!SyncRules::GetSingleton()->ShouldSyncGraphVariable(name, type)) {
            SKSE::log::warn(
                "GraphVarRx rejected sender={} name='{}' type={} reason='not in sync rules'",
                senderConnectionID,
                name,
                variableType);
            return;
        }

        const auto formID = Resolve(senderConnectionID);
        if (!formID) return;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
        if (!actor) {
            SKSE::log::info(
                "GraphVarRx sender={} proxy={:08X} actor='<missing>' name='{}' applied=false",
                senderConnectionID,
                formID,
                name);
            return;
        }

        const RE::BSFixedString variableName{ name.c_str() };
        bool applied = false;

        switch (type) {
        case GraphVariableType::kBool:
            applied = actor->SetGraphVariableBool(variableName, boolValue);
            SKSE::log::info(
                "GraphVarRx sender={} proxy={:08X} actor='{}' name='{}' type=bool value={} applied={}",
                senderConnectionID,
                formID,
                actor->GetName(),
                name,
                boolValue,
                applied);
            break;

        case GraphVariableType::kInt:
            applied = actor->SetGraphVariableInt(variableName, intValue);
            SKSE::log::info(
                "GraphVarRx sender={} proxy={:08X} actor='{}' name='{}' type=int value={} applied={}",
                senderConnectionID,
                formID,
                actor->GetName(),
                name,
                intValue,
                applied);
            break;

        case GraphVariableType::kFloat:
            applied = actor->SetGraphVariableFloat(variableName, floatValue);
            SKSE::log::info(
                "GraphVarRx sender={} proxy={:08X} actor='{}' name='{}' type=float value={:.6f} applied={}",
                senderConnectionID,
                formID,
                actor->GetName(),
                name,
                floatValue,
                applied);
            break;
        }

        if (applied) {
            OARClient::GetSingleton()->ClearConditionStateData(actor);
        }
    }

}
