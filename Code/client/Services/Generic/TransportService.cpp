
#include <Services/TransportService.h>

#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/ServerLobbyEvent.h>
#include <ModCompat/CharacterCheckpoints.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>

#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>

#include <TimeManager.h>

#include <Forms/TESNPC.h>
#include <TiltedOnlinePCH.h>
#include <World.h>

#include <Messages/AuthenticationRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/NotifySettingsChange.h>
#include <Packet.hpp>
#include <steam/isteamnetworkingutils.h>

#include <ScriptExtender.h>
#include <Services/DiscordService.h>

// #include <imgui_internal.h>

static constexpr wchar_t kMO2DllName[] = L"usvfs_x64.dll";

using TiltedPhoques::Packet;

TransportService::TransportService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&TransportService::HandleUpdate>(this);
    m_settingsChangeConnection = m_dispatcher.sink<NotifySettingsChange>().connect<&TransportService::HandleNotifySettingsChange>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&TransportService::HandleConnected>(this);
    m_disconnectedConnection = m_dispatcher.sink<DisconnectedEvent>().connect<&TransportService::HandleDisconnected>(this);

    m_connected = false;

    auto handlerGenerator = [this](auto& x)
    {
        using T = typename std::remove_reference_t<decltype(x)>::Type;

        m_messageHandlers[T::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
        {
            const auto pRealMessage = TiltedPhoques::CastUnique<T>(std::move(apMessage));
            m_dispatcher.trigger(*pRealMessage);
        };

        return false;
    };

    ServerMessageFactory::Visit(handlerGenerator);

    m_messageHandlers[NotifyCharacterSave::Opcode] = [](UniquePtr<ServerMessage>& apMessage)
    {
        CharacterCheckpoints::Receive(*TiltedPhoques::CastUnique<NotifyCharacterSave>(std::move(apMessage)));
    };

    // Override authentication response
    m_messageHandlers[AuthenticationResponse::Opcode] = [this](UniquePtr<ServerMessage>& apMessage)
    {
        const auto pRealMessage = TiltedPhoques::CastUnique<AuthenticationResponse>(std::move(apMessage));
        HandleAuthenticationResponse(*pRealMessage);
    };
}

bool TransportService::Send(const ClientMessage& acMessage, const TiltedPhoques::EPacketFlags aPacketFlags) const noexcept
{
    static thread_local ScratchAllocator s_allocator(1 << 18);

    struct ScopedReset
    {
        ~ScopedReset() { s_allocator.Reset(); }
    } allocatorGuard;

    if (Client::IsConnected() && (m_connected || acMessage.GetOpcode() == AuthenticationRequest::Opcode ||
        (m_lobbyAuthenticated && acMessage.GetOpcode() == RequestCharacterSave::Opcode)))
    {
        ScopedAllocator _{s_allocator};

        Buffer buffer(1 << 16);
        Buffer::Writer writer(&buffer);
        writer.WriteBits(0, 8); // Write first byte as packet needs it

        acMessage.Serialize(writer);
        TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), writer.Size());

        Client::Send(&packet, aPacketFlags);

        return true;
    }

    return false;
}

void TransportService::OnConsume(const void* apData, uint32_t aSize)
{
    if (!aSize || (static_cast<const uint8_t*>(apData)[0] == NotifyCharacterSave::Opcode && aSize != CharacterSavePayload::WireSize))
        return;
    ServerMessageFactory factory;
    TiltedPhoques::ViewBuffer buf((uint8_t*)apData, aSize);
    Buffer::Reader reader(&buf);

    auto pMessage = factory.Extract(reader);
    if (!pMessage)
    {
        spdlog::error("Couldn't parse packet from server");
        return;
    }

    if (m_connected || pMessage->GetOpcode() == AuthenticationResponse::Opcode ||
        (m_lobbyAuthenticated && pMessage->GetOpcode() == NotifyCharacterSave::Opcode))
        m_messageHandlers[pMessage->GetOpcode()](pMessage);
}

bool TransportService::CanEnterWorld() const noexcept
{
    auto* pUI = UI::Get();
    if (!pUI || pUI->GetMenuOpen("Main Menu") || pUI->GetMenuOpen("Loading Menu") || pUI->GetMenuOpen("RaceSex Menu"))
        return false;
    auto* pPlayer = PlayerCharacter::Get();
    auto* pTime = TimeData::Get();
    return pPlayer && pPlayer->parentCell && pPlayer->GetNiNode() && pPlayer->baseForm &&
        ModManager::Get() && pTime && pTime->TimeScale && pTime->GameHour &&
        pTime->GameYear && pTime->GameMonth && pTime->GameDay;
}

void TransportService::BeginConnection(const std::string& acEndpoint, const std::string& acPassword) noexcept
{
    if (m_connectionPending || m_connected || m_lobbyAuthenticated || m_enterPending || Client::IsConnected())
        return;
    m_enterAfterLogin = CanEnterWorld();
    m_connectionPending = true;
    SetServerPassword(acPassword);
    if (!Client::Connect(acEndpoint))
        OnDisconnected(kLocalProblem);
}

void TransportService::OnConnected()
{
    // Retain support for existing debug callers that use the base Connect
    // API rather than the overlay's BeginConnection.
    if (!m_connectionPending)
    {
        m_connectionPending = true;
        m_enterAfterLogin = CanEnterWorld();
    }
    AuthenticationRequest request{};
    request.Phase = AuthenticationRequest::Login;
    request.Version = BUILD_PROTOCOL;
    request.SKSEActive = IsScriptExtenderLoaded();
    request.MO2Active = GetModuleHandleW(kMO2DllName) != nullptr;
    request.Token = m_serverPassword;
    m_serverPassword.clear();
    request.DiscordId = m_world.ctx().at<DiscordService>().GetUser().id;
    Send(request);
}

bool TransportService::EnterWorld() noexcept
{
    if (!m_lobbyAuthenticated || m_enterPending || !CanEnterWorld() || !CharacterCheckpoints::CanEnter())
        return false;

    AuthenticationRequest request{};
    request.Phase = AuthenticationRequest::EnterWorld;
    request.Version = BUILD_PROTOCOL;
    request.SKSEActive = IsScriptExtenderLoaded();
    request.MO2Active = GetModuleHandleW(kMO2DllName) != nullptr;
    auto* pPlayer = PlayerCharacter::Get();
    auto* pNpc = Cast<TESNPC>(pPlayer->baseForm);
    if (!pNpc || !pNpc->fullName.value.AsAscii())
        return false;
    request.Username = pNpc->fullName.value.AsAscii();
    if (request.Username.empty() || request.Username.size() > 128)
        return false;

    for (auto* pMod : ModManager::Get()->mods)
    {
        if (!pMod || !pMod->IsLoaded())
            continue;
        auto& entry = request.UserMods.ModList.emplace_back();
        entry.Id = pMod->GetId();
        entry.IsLite = pMod->IsLite();
        entry.Filename = pMod->filename;
    }
    const auto localLocation = [](uint32_t aFormId)
    {
        const uint32_t index = aFormId >> 24;
        if (index == 0xFE)
            return GameId(0x10000u | ((aFormId >> 12) & 0xFFFu), aFormId & 0xFFFu);
        return GameId(index, aFormId & 0xFFFFFFu);
    };
    if (pPlayer->GetWorldSpace())
        request.WorldSpaceId = localLocation(pPlayer->GetWorldSpace()->formID);
    request.CellId = localLocation(pPlayer->parentCell->formID);
    request.Level = pPlayer->GetLevel();

    auto* pGameTime = TimeData::Get();
    request.PlayerTime.TimeScale = pGameTime->TimeScale->f;
    request.PlayerTime.Time = pGameTime->GameHour->f;
    request.PlayerTime.Year = pGameTime->GameYear->f;
    request.PlayerTime.Month = pGameTime->GameMonth->f;
    request.PlayerTime.Day = pGameTime->GameDay->f;

    m_enterPending = Send(request);
    return m_enterPending;
}

void TransportService::OnDisconnected(EDisconnectReason aReason)
{
    const bool wasInWorld = m_connected;
    m_connected = false;
    m_connectionPending = false;
    m_lobbyAuthenticated = false;
    m_enterPending = false;
    m_enterAfterLogin = false;
    m_serverPassword.clear();
    m_localPlayerId = 0;
    CharacterCheckpoints::Reset();
    spdlog::warn("Disconnected from server {}", aReason);
    if (wasInWorld)
        m_dispatcher.trigger(DisconnectedEvent());
    else
        m_dispatcher.trigger(ServerLobbyEvent{false});
}

void TransportService::OnUpdate()
{
    CharacterCheckpoints::Tick(*this);
}

double TransportService::GetMessageReceiveTime() const noexcept
{
    return static_cast<double>(GetCurrentMessageReceiveTime()) / 1000.0;
}

void TransportService::HandleUpdate(const UpdateEvent&) noexcept
{
    Poll();
}

void TransportService::Poll() noexcept
{
    // Taken before polling, so that every message that arrived before this time gets handled by this update
    m_lastPollTime = static_cast<double>(SteamNetworkingUtils()->GetLocalTimestamp()) / 1000.0;

    Update();
}

void TransportService::HandleConnected(const ConnectedEvent& acEvent) noexcept
{
    m_localPlayerId = acEvent.PlayerId;
}

void TransportService::HandleDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    m_localPlayerId = NULL;
}

void TransportService::HandleAuthenticationResponse(const AuthenticationResponse& acMessage) noexcept
{
    using AR = AuthenticationResponse::ResponseType;
    if (acMessage.Type == AR::kLobbyAccepted && m_connectionPending && !m_lobbyAuthenticated)
    {
        m_connectionPending = false;
        m_lobbyAuthenticated = true;
        CharacterCheckpoints::Begin(acMessage.RequiresCharacterSave);
        if (!acMessage.RequiresCharacterSave && m_enterAfterLogin && EnterWorld())
            return;
        m_dispatcher.trigger(ServerLobbyEvent{true});
        return;
    }
    if (acMessage.Type == AR::kAccepted && m_lobbyAuthenticated && m_enterPending)
    {
        if (!CanEnterWorld())
        {
            Close();
            return;
        }
        m_lobbyAuthenticated = false;
        m_enterPending = false;
        m_enterAfterLogin = false;
        m_connected = true;
        m_localPlayerId = acMessage.PlayerId;

        m_world.SetServerSettings(acMessage.Settings);

        m_dispatcher.trigger(acMessage.UserMods);
        m_dispatcher.trigger(acMessage.Settings);
        m_dispatcher.trigger(ConnectedEvent(acMessage.PlayerId));
        return; // quit the function here.
    }

    // error finding

    TiltedPhoques::String ErrorInfo;

    ErrorInfo = "{";

    switch (acMessage.Type)
    {
    case AR::kWrongVersion:
        ErrorInfo += "\"error\": \"wrong_version\", \"data\": {";
        ErrorInfo += fmt::format("\"expectedVersion\": \"{}\", \"version\": \"{}\"", acMessage.Version, BUILD_PROTOCOL);
        ErrorInfo += "}";
        break;
    case AR::kModsMismatch:
    {
        ErrorInfo += "\"error\": \"mods_mismatch\", \"data\": {\"mods\": [";
        bool first = true;
        for (const auto& m : acMessage.UserMods.ModList)
        {
            if (!first)
                ErrorInfo += ",";
            ErrorInfo += fmt::format("[\"{}\",\"{}\"]", m.Filename.c_str(), m.Id);
            first = false;
        }
        ErrorInfo += "]}";
        break;
    }
    case AR::kClientModsDisallowed:
    {
        ErrorInfo += "\"error\": \"client_mods_disallowed\", \"data\": { \"mods\": [";
        if (acMessage.SKSEActive)
            ErrorInfo += "\"SKSE\"";
        if (acMessage.MO2Active)
            if (acMessage.SKSEActive)
                ErrorInfo += ",";
        ErrorInfo += "\"MO2\"";
        ErrorInfo += "]}";
        break;
    }
    case AR::kWrongPassword:
    {
        ErrorInfo += "\"error\": \"wrong_password\"";
        break;
    }
    case AR::kServerFull:
    {
        ErrorInfo += "\"error\": \"server_full\"";
        break;
    }
    default: ErrorInfo += "\"error\": \"no_reason\""; break;
    }

    ErrorInfo += "}";

    ConnectionErrorEvent errorEvent;
    if (!ErrorInfo.empty())
    {
        spdlog::error(ErrorInfo.c_str());
        errorEvent.ErrorDetail = std::move(ErrorInfo);
    }

    m_dispatcher.trigger(errorEvent);
}

void TransportService::HandleNotifySettingsChange(const NotifySettingsChange& acMessage) noexcept
{
    m_world.SetServerSettings(acMessage.Settings);
    m_dispatcher.trigger(acMessage.Settings);
}
