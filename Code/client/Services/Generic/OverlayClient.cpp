#include <TiltedOnlinePCH.h>

#include <OverlayRenderHandler.hpp>
#include <DInputHook.hpp>

#include <Services/OverlayClient.h>
#include <Services/InputService.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <ModCompat/CharacterCheckpoints.h>

#include <Messages/SendChatMessageRequest.h>
#include <Messages/PlayerDialogueRequest.h>
#include <limits>
#include <cmath>
#include <Messages/TeleportRequest.h>

#include <Events/SetTimeCommandEvent.h>

#include <World.h>

OverlayClient::OverlayClient(TransportService& aTransport, TiltedPhoques::OverlayRenderHandler* apHandler)
    : TiltedPhoques::OverlayClient(apHandler)
    , m_transport(aTransport)
{
}

OverlayClient::~OverlayClient() noexcept
{
}

void OverlayClient::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
    TiltedPhoques::OverlayClient::OnAfterCreated(browser);
    spdlog::info("Skyrim Together CEF browser created");
}

bool OverlayClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefProcessId source_process, CefRefPtr<CefProcessMessage> message)
{
    if (message->GetName() == "ui-event")
    {
        auto pArguments = message->GetArgumentList();

        auto eventName = pArguments->GetString(0).ToString();
        auto eventArgs = pArguments->GetList(1);

        // Never log arbitrary UI arguments; a connect request contains a password.
        spdlog::debug("UI event {}", eventName);

#ifndef PUBLIC_BUILD
        LOG(INFO) << "event=ui_event name=" << eventName;
#endif

        if (eventName == "uiReady")
        {
            spdlog::info("Skyrim Together frontend uiReady received");
            World::Get().GetOverlayService().SignalBrowserReady();
        }
        else if (eventName == "connect")
            ProcessConnectMessage(eventArgs);
        else if (eventName == "disconnect")
            ProcessDisconnectMessage();
        else if (eventName == "selectCharacter")
        {
            const std::string key = eventArgs->GetString(0).ToString();
            World::Get().GetRunner().Queue([key]()
            {
                CharacterCheckpoints::Select(World::Get().GetTransport(), key);
            });
        }
        else if (eventName == "enterWorld")
        {
            World::Get().GetRunner().Queue([]()
            {
                auto& world = World::Get();
                if (CharacterCheckpoints::FinishCharacter(world.GetTransport())) return;
                if (!world.GetTransport().EnterWorld())
                    world.GetOverlayService().SendSystemMessage("Finish loading or creating your character, then try Enter World again.");
            });
        }
        else if (eventName == "deactivate")
            World::Get().GetRunner().Queue([]() { InputService::SetOverlayActive(World::Get().GetOverlayService(), false); });
        else if (!m_transport.IsOnline() && eventName != "openPlayGuide")
            return true;
        else if (eventName == "revealPlayers")
            ProcessRevealPlayersMessage();
        else if (eventName == "sendMessage")
            ProcessChatMessage(eventArgs);
        else if (eventName == "voteDialogue" || eventName == "manageDialogue")
        {
            // Send numeric arguments through the existing CEF event bridge, not chat.
            const auto readId = [&](size_t aIndex, uint32_t& aValue)
            {
                const double value = eventArgs->GetType(aIndex) == VTYPE_DOUBLE
                    ? eventArgs->GetDouble(aIndex) : static_cast<double>(eventArgs->GetInt(aIndex));
                if (value < 0 || value > std::numeric_limits<uint32_t>::max() || !std::isfinite(value) || std::floor(value) != value)
                    return false;
                aValue = static_cast<uint32_t>(value);
                return true;
            };
            PlayerDialogueRequest request{};
            if (eventArgs->GetSize() == 3 &&
                readId(0, request.ActorId) && readId(1, request.Revision) && readId(2, request.Index))
            {
                if (eventName == "manageDialogue")
                {
                    World::Get().GetRunner().Queue([request]()
                    {
                        World::Get().GetOverlayService().ManageDialogue(request.ActorId, request.Revision, request.Index);
                    });
                }
                else
                {
                    request.Action = PlayerDialogueRequest::Vote;
                    World::Get().GetRunner().Queue([request]() { World::Get().GetTransport().Send(request); });
                }
            }
        }
        else if (eventName == "setTime")
            ProcessSetTimeCommand(eventArgs);
        else if (eventName == "launchParty")
            World::Get().GetPartyService().CreateParty();
        else if (eventName == "leaveParty")
            World::Get().GetPartyService().LeaveParty();
        else if (eventName == "createPartyInvite")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().CreateInvite(aPlayerId);
        }
        else if (eventName == "acceptPartyInvite")
        {
            uint32_t aInviterId = eventArgs->GetInt(0);
            // push to main thread because the party service has to check validity of invite thread safely
            World::Get().GetRunner().Queue([aInviterId]() { World::Get().GetPartyService().AcceptInvite(aInviterId); });
        }
        else if (eventName == "kickPartyMember")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().KickPartyMember(aPlayerId);
        }
        else if (eventName == "changePartyLeader")
        {
            uint32_t aPlayerId = eventArgs->GetInt(0);
            World::Get().GetPartyService().ChangePartyLeader(aPlayerId);
        }
        else if (eventName == "teleportToPlayer")
            ProcessTeleportMessage(eventArgs);
        else if (eventName == "toggleDebugUI")
            ProcessToggleDebugUI();
        else if (eventName == "openPlayGuide")
            ProcessOpenPlayGuide();

        return true;
    }

    return false;
}

void OverlayClient::ProcessConnectMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string baseIp = aEventArgs->GetString(0);
    if (baseIp == "localhost")
    {
        baseIp = "127.0.0.1";
    }

    uint16_t port = aEventArgs->GetInt(1) ? aEventArgs->GetInt(1) : 10578;
    const std::string password = aEventArgs->GetString(2).ToString();
    const std::string endpoint = baseIp + ":" + std::to_string(port);
    World::Get().GetRunner().Queue([endpoint, password] { World::Get().GetTransport().BeginConnection(endpoint, password); });
}

void OverlayClient::ProcessDisconnectMessage()
{
    World::Get().GetRunner().Queue([]() { World::Get().GetTransport().Close(); });
}

void OverlayClient::ProcessRevealPlayersMessage()
{
    SetUIVisible(false);
    World::Get().GetMagicService().StartRevealingOtherPlayers();
}

void OverlayClient::ProcessChatMessage(CefRefPtr<CefListValue> aEventArgs)
{
    std::string contents = aEventArgs->GetString(1).ToString();
    if (!contents.empty())
    {
        SendChatMessageRequest messageRequest;
        messageRequest.MessageType = static_cast<ChatMessageType>(aEventArgs->GetInt(0));
        messageRequest.ChatMessage = contents;

        spdlog::info(L"Send chat message of type {}: '{}' ", messageRequest.MessageType, aEventArgs->GetString(1).ToWString());

        m_transport.Send(messageRequest);
    }
}

void OverlayClient::ProcessSetTimeCommand(CefRefPtr<CefListValue> aEventArgs)
{
    const uint8_t hours = static_cast<uint8_t>(aEventArgs->GetInt(0));
    const uint8_t minutes = static_cast<uint8_t>(aEventArgs->GetInt(1));
    const uint32_t senderId = m_transport.GetLocalPlayerId();
    World::Get().GetDispatcher().trigger(SetTimeCommandEvent(hours, minutes, senderId));
}

void OverlayClient::ProcessTeleportMessage(CefRefPtr<CefListValue> aEventArgs)
{
    TeleportRequest request{};
    request.PlayerId = aEventArgs->GetInt(0);

    m_transport.Send(request);
}

void OverlayClient::ProcessToggleDebugUI()
{
    World::Get().GetDebugService().m_showDebugStuff = !World::Get().GetDebugService().m_showDebugStuff;
}

void OverlayClient::ProcessOpenPlayGuide()
{
    ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/general-information/playguide)", nullptr, nullptr, SW_SHOWNORMAL);
}

void OverlayClient::SetUIVisible(bool aVisible) noexcept
{
    auto pRenderer = GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    TiltedPhoques::DInputHook::Get().SetEnabled(aVisible);
    World::Get().GetOverlayService().SetActive(aVisible);
    pRenderer->SetCursorVisible(aVisible);
}
