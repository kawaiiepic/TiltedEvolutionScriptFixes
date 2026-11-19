#include "PCH.h"
#include "NativeAdapter.h"
#include "LocalCaptureProbe.h"
#include "RemoteIEDRenderer.h"
#include <Windows.h>
#include <cstring>
#include <cmath>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr char kChannel[] = "skyrimtogether.ied.state.v1";
        constexpr std::size_t kChunkSize = 4000;
        constexpr std::size_t kMaxSnapshot = 24 * 1024;
        constexpr std::size_t kMaxPeers = 64;
        constexpr std::size_t kMaxParts = (kMaxSnapshot + kChunkSize - 1) / kChunkSize;
        struct Header
        {
            std::uint32_t revision;
            std::uint16_t part, count;
        };
        static_assert(sizeof(Header) == 8);
        static_assert(sizeof(Header) + kChunkSize <= SkyrimTogether::MaxPayloadSize);

        bool ValidText(std::string_view text, std::size_t limit)
        {
            return text.size() <= limit && text.find('\0') == text.npos;
        }

        bool ValidState(const LocalIEDState& state)
        {
            const auto validForm = [](const FormIdentity& form) {
                return !form.plugin.empty() && ValidText(form.plugin, 260) &&
                    form.localFormID != 0 && form.localFormID <= 0xFFFFFF;
            };
            for (const auto& slot : state.slots)
                if (slot && !validForm(*slot)) return false;
            for (const auto& object : state.objects) {
                if (!validForm(object.form) || !ValidText(object.objectNode, 512) ||
                    !ValidText(object.attachmentNode, 512) || !ValidText(object.anchorNode, 512) ||
                    !std::isfinite(object.scale) || object.scale < 0 || object.scale > 100)
                    return false;
                for (const auto value : object.position)
                    if (!std::isfinite(value) || std::abs(value) > 10000) return false;
                for (const auto value : object.rotationMatrix)
                    if (!std::isfinite(value) || std::abs(value) > 1.01f) return false;
            }
            return true;
        }
    }

    NativeAdapter& NativeAdapter::GetSingleton()
    {
        static NativeAdapter instance;
        return instance;
    }

    void NativeAdapter::Start()
    {
        if (_timer.joinable()) return;
        _timer = std::jthread([this](std::stop_token token) {
            while (!token.stop_requested()) {
                if (auto* tasks = SKSE::GetTaskInterface(); tasks && !_queued.exchange(true)) {
                    tasks->AddTask([this]() {
                        Tick();
                        _queued.store(false);
                    });
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
    }

    bool NativeAdapter::Discover()
    {
        if (_api) return true;
        const auto now = Clock::now();
        if (now < _nextDiscovery) return false;
        _nextDiscovery = now + std::chrono::seconds(5);
        auto module = GetModuleHandleW(L"SkyrimTogether.exe");
        if (!module) module = GetModuleHandleW(nullptr);
        const auto query = reinterpret_cast<SkyrimTogether::QueryPluginMessaging>(
            GetProcAddress(module, SkyrimTogether::PluginMessagingExport));
        if (!query) return false;
        const SkyrimTogether::PluginMessagingInterface* api{};
        using Result = SkyrimTogether::PluginMessageResult;
        if (query(SkyrimTogether::PluginMessagingVersion, &api) != Result::Ok ||
            !api || !api->Register || !api->Send || !api->LocalPlayerId || !api->ResolvePlayerFormId)
            return false;
        if (api->Register(kChannel, &Receive, this) != Result::Ok) return false;
        _api = api;
        SKSE::log::info("IED native messaging registered: {}", kChannel);
        return true;
    }

    void NativeAdapter::Reset()
    {
        RemoteIEDRenderer::GetSingleton().Reset();
        _peers.clear();
        _local = 0;
        _lastSent.clear();
        _nextSend = {};
        _heartbeat = {};
    }

    void NativeAdapter::UpdateSession(std::uint32_t id)
    {
        if (_local == id) return;
        Reset();
        _local = id;
    }

    void NativeAdapter::Receive(std::uint32_t sender, const void* data, std::size_t size, void* context)
    {
        auto& self = *static_cast<NativeAdapter*>(context);
        if (!self._api) return;
        self.UpdateSession(self._api->LocalPlayerId());
        if (!self._local || !sender || sender == self._local) return;
        self.Accept(sender, data, size);
    }

    void NativeAdapter::Accept(std::uint32_t sender, const void* data, std::size_t size)
    {
        if (!data || size <= sizeof(Header) || size > sizeof(Header) + kChunkSize) return;
        Header header{};
        std::memcpy(&header, data, sizeof(header));
        const auto length = size - sizeof(Header);
        if (!header.revision || !header.count || header.count > kMaxParts || header.part >= header.count ||
            (header.part + 1 < header.count && length != kChunkSize)) return;
        if (!_peers.contains(sender)) {
            if (header.part || _peers.size() >= kMaxPeers) return;
            _peers.try_emplace(sender).first->second.seen = Clock::now();
        }
        auto& peer = _peers.at(sender);
        if (!header.part && header.revision > peer.revision) {
            peer.revision = header.revision;
            peer.count = header.count;
            peer.next = 0;
            peer.partial.clear();
        }
        if (header.revision != peer.revision || header.count != peer.count || header.part != peer.next ||
            peer.partial.size() + length > kMaxSnapshot) return;
        peer.partial.append(static_cast<const char*>(data) + sizeof(Header), length);
        if (++peer.next != peer.count) return;
        auto state = DecodeLocalIEDState(peer.partial);
        peer.partial.clear();
        if (!state || !ValidState(*state)) return;
        peer.seen = Clock::now();
        if (!peer.state || *peer.state != *state) {
            peer.state = std::move(state);
            peer.dirty = true;
        }
    }

    bool NativeAdapter::SendSnapshot(std::string_view payload)
    {
        if (payload.empty() || payload.size() > kMaxSnapshot) return false;
        if (++_revision == 0) ++_revision;
        const auto count = static_cast<std::uint16_t>((payload.size() + kChunkSize - 1) / kChunkSize);
        for (std::uint16_t part = 0; part < count; ++part) {
            const auto offset = static_cast<std::size_t>(part) * kChunkSize;
            const auto length = std::min(kChunkSize, payload.size() - offset);
            const Header header{_revision, part, count};
            std::array<char, sizeof(Header) + kChunkSize> packet{};
            std::memcpy(packet.data(), &header, sizeof(header));
            std::memcpy(packet.data() + sizeof(Header), payload.data() + offset, length);
            if (_api->Send(kChannel, SkyrimTogether::Broadcast, packet.data(), sizeof(Header) + length) !=
                SkyrimTogether::PluginMessageResult::Ok) return false;
        }
        return true;
    }

    void NativeAdapter::Tick()
    {
        if (!Discover()) return;
        UpdateSession(_api->LocalPlayerId());
        if (!_local) return;
        const auto now = Clock::now();
        if (now >= _nextSend) {
            _nextSend = now + std::chrono::seconds(2);
            const auto payload = LocalCaptureProbe::GetSingleton().GetLastPayload();
            if (!payload.empty() && (payload != _lastSent || now >= _heartbeat)) {
                if (payload.size() > kMaxSnapshot) {
                    if (now >= _heartbeat) {
                        SKSE::log::warn("IED snapshot exceeds native 24 KiB limit: {}", payload.size());
                        _heartbeat = now + std::chrono::seconds(10);
                    }
                } else if (SendSnapshot(payload)) {
                    _lastSent = payload;
                    _heartbeat = now + std::chrono::seconds(10);
                }
            }
        }
        auto& renderer = RemoteIEDRenderer::GetSingleton();
        for (auto it = _peers.begin(); it != _peers.end();) {
            auto& [id, peer] = *it;
            if (now - peer.seen > std::chrono::seconds(40)) {
                if (peer.proxy) renderer.ClearProxy(peer.proxy);
                it = _peers.erase(it);
                continue;
            }
            const auto proxy = _api->ResolvePlayerFormId(id);
            if (peer.proxy != proxy) {
                if (peer.proxy) renderer.ClearProxy(peer.proxy);
                peer.proxy = proxy;
                peer.dirty = true;
            }
            if (proxy && peer.state && peer.dirty)
                peer.dirty = !renderer.Apply(id, proxy, "", *peer.state, true);
            ++it;
        }
    }
}
