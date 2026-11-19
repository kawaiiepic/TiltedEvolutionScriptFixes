#include <TiltedOnlinePCH.h>

#include <Services/OverlayService.h>
#include <Services/InputService.h>
#include <ModCompat/DialoguePortraits.h>
#include <ModCompat/CharacterCheckpoints.h>
#include <ModCompat/MainMenuBridge.h>

#include <OverlayApp.hpp>

#include <D3D11Hook.hpp>
#include <OverlayRenderHandlerD3D11.hpp>

#include <Systems/RenderSystemD3D11.h>
#include <Games/Skyrim/BSGraphics/BSGraphicsRenderer.h>

#include <World.h>

#include <Services/OverlayClient.h>
#include <Services/TransportService.h>

#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyTravelInvitation.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerDialogue.h>
#include <Messages/PlayerDialogueRequest.h>
#include <Games/Misc/MenuTopicManager.h>
#include <TESObjectREFR.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/RequestPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerHealthUpdate.h>

#include <Structs/GridCellCoords.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/ServerLobbyEvent.h>
#include <Games/Skyrim/Interface/UI.h>
#include <DInputHook.hpp>
#include <Events/ConnectionErrorEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>

#include <PlayerCharacter.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>

using TiltedPhoques::OverlayRenderHandler;
using TiltedPhoques::OverlayRenderHandlerD3D11;

namespace
{
struct SkyrimOverlayApp final : OverlayApp
{
    using OverlayApp::OverlayApp;

    void OnBeforeCommandLineProcessing(const CefString& aProcessType, CefRefPtr<CefCommandLine> aCommandLine) override
    {
        OverlayApp::OnBeforeCommandLineProcessing(aProcessType, aCommandLine);
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll && GetProcAddress(ntdll, "wine_get_version"))
        {
            aCommandLine->AppendSwitch("disable-gpu");
            aCommandLine->AppendSwitch("disable-gpu-compositing");
            if (aProcessType.empty())
                spdlog::info("Overlay: CEF draws the UI on the CPU under Wine");
        }
    }
};
}

struct D3D11RenderProvider final : OverlayApp::RenderProvider, OverlayRenderHandlerD3D11::Renderer
{
    explicit D3D11RenderProvider(RenderSystemD3D11* apRenderSystem)
        : m_pRenderSystem(apRenderSystem)
    {
    }

    OverlayRenderHandler* Create() override
    {
        auto* pHandler = new OverlayRenderHandlerD3D11(this);
        pHandler->SetVisible(true);

        return pHandler;
    }

    [[nodiscard]] HWND GetWindow() override { return m_pRenderSystem->GetWindow(); }

    [[nodiscard]] IDXGISwapChain* GetSwapChain() const noexcept override { return m_pRenderSystem->GetSwapChain(); }
    [[nodiscard]] ID3D11Device* GetDevice() const noexcept override { return m_pRenderSystem->GetDevice(); }
    [[nodiscard]] ID3D11DeviceContext* GetDeviceContext() const noexcept override { return m_pRenderSystem->GetDeviceContext(); }

private:
    RenderSystemD3D11* m_pRenderSystem;
};

String GetCellName(const GameId& aWorldSpaceId, const GameId& aCellId) noexcept
{
    auto& modSystem = World::Get().GetModSystem();

    String cellName = "UNKNOWN";

    if (aWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(aWorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
            if (const char* pName = pWorldSpace->GetName(); pName && *pName)
                cellName = pName;
    }
    else
    {
        const uint32_t cellId = modSystem.GetGameId(aCellId);
        TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
        if (pCell)
            if (const char* pName = pCell->GetName(); pName && *pName)
                cellName = pName;
    }

    return cellName;
}

float CalculateHealthPercentage(Actor* apActor) noexcept
{
    const float maxHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float tempModHealth = apActor->healthModifiers.temporaryModifier;

    if (maxHealth == 0.f)
        return 0.f;
    const float health = apActor->GetActorValue(ActorValueInfo::kHealth);

    float percentage = health / (maxHealth + tempModHealth) * 100.f;
    if (percentage < 0.f)
        percentage = 0.f;

    return percentage;
}

OverlayService::OverlayService(World& aWorld, TransportService& transport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(transport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&OverlayService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&OverlayService::OnConnectedEvent>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&OverlayService::OnDisconnectedEvent>(this);
    m_lobbyConnection = aDispatcher.sink<ServerLobbyEvent>().connect<&OverlayService::OnServerLobbyEvent>(this);
    m_connectionErrorConnection = aDispatcher.sink<ConnectionErrorEvent>().connect<&OverlayService::OnConnectionError>(this);
    m_travelInvitationConnection = aDispatcher.sink<NotifyTravelInvitation>().connect<&OverlayService::OnTravelInvitation>(this);
    m_chatMessageConnection = aDispatcher.sink<NotifyChatMessageBroadcast>().connect<&OverlayService::OnChatMessageReceived>(this);
    m_playerJoinedConnection = aDispatcher.sink<NotifyPlayerJoined>().connect<&OverlayService::OnPlayerJoined>(this);
    m_playerLeftConnection = aDispatcher.sink<NotifyPlayerLeft>().connect<&OverlayService::OnPlayerLeft>(this);
    m_playerDialogueConnection = aDispatcher.sink<NotifyPlayerDialogue>().connect<&OverlayService::OnPlayerDialogue>(this);
    m_playerAddedConnection = m_world.on_destroy<WaitingFor3D>().connect<&OverlayService::OnWaitingFor3DRemoved>(this);
    m_playerRemovedConnection = m_world.on_destroy<PlayerComponent>().connect<&OverlayService::OnPlayerComponentRemoved>(this);
    m_playerLevelConnection = aDispatcher.sink<NotifyPlayerLevel>().connect<&OverlayService::OnPlayerLevel>(this);
    m_cellChangedConnection = aDispatcher.sink<NotifyPlayerCellChanged>().connect<&OverlayService::OnPlayerCellChanged>(this);
    m_playerHealthConnection = aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&OverlayService::OnNotifyPlayerHealthUpdate>(this);
    m_partyJoinedConnection = aDispatcher.sink<PartyJoinedEvent>().connect<&OverlayService::OnPartyJoinedEvent>(this);
    m_partyLeftConnection = aDispatcher.sink<PartyLeftEvent>().connect<&OverlayService::OnPartyLeftEvent>(this);
}

OverlayService::~OverlayService() noexcept
{
    DialoguePortraits::Reset();
}

void OverlayService::Create(RenderSystemD3D11* apRenderSystem) noexcept
{
    m_pProvider = TiltedPhoques::MakeUnique<D3D11RenderProvider>(apRenderSystem);
    m_pOverlay = new SkyrimOverlayApp(m_pProvider.get(), new ::OverlayClient(m_transport, m_pProvider->Create()));

    spdlog::info("Initializing Skyrim Together browser overlay");
    m_browserReady = false;
    if (!m_pOverlay->Initialize())
    {
        spdlog::error("Overlay could not be initialized");
        if (int32_t exitCode = CefGetExitCode())
        {
            spdlog::critical("CEF failed to initialize, exit code {}. See 'cef_types.h' for description", exitCode);
        }
    }

    else
        spdlog::info("Skyrim Together browser creation requested");

    m_pOverlay->GetClient()->Create();
    spdlog::info("Skyrim Together overlay render resources initialized");
}

void OverlayService::Render() noexcept
{
    auto pPlayer = PlayerCharacter::Get();
    bool inGame = pPlayer && pPlayer->GetNiNode();
    if (inGame && !m_inGame)
        SetInGame(true);
    else if (!inGame && m_inGame)
        SetInGame(false);

    m_pOverlay->GetClient()->Render();
}

void OverlayService::Reset() const noexcept
{
    m_pOverlay->GetClient()->Reset();
}

void OverlayService::Reload() noexcept
{
    SetInGame(false);
    SetActive(false);
    m_browserReady = false;
    GetOverlayApp()->GetClient()->GetBrowser()->Reload();
    Initialize();
    SetInGame(true);
    m_pOverlay->ExecuteAsync("enterGame");
    SetActive(true);
}

void OverlayService::Initialize() noexcept
{
    m_pOverlay->ExecuteAsync("init");
}

void OverlayService::SetActive(bool aActive) noexcept
{
    if (aActive && !m_inGame && !m_mainMenu)
        return;
    if (m_active == aActive)
        return;

    m_active = aActive;

    m_pOverlay->ExecuteAsync(m_active ? "activate" : "deactivate");
}

bool OverlayService::GetActive() const noexcept
{
    return m_active;
}

void OverlayService::SetInGame(bool aInGame) noexcept
{
    if (m_inGame == aInGame)
        return;
    m_inGame = aInGame;

    if (m_inGame)
    {
        SetVersion(BUILD_COMMIT);
        m_pOverlay->ExecuteAsync("enterGame");
    }
    else
    {
        m_pOverlay->ExecuteAsync("exitGame");
        if (!m_mainMenu && m_active)
        {
            SetActive(false);
            TiltedPhoques::DInputHook::Get().SetEnabled(false);
            if (const auto client = m_pOverlay->GetClient())
                if (const auto renderer = client->GetOverlayRenderHandler())
                    renderer->SetCursorVisible(false);
        }
    }
}

bool OverlayService::GetInGame() const noexcept
{
    return m_inGame;
}

void OverlayService::SetVersion(const std::string& acVersion)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();

    pArguments->SetString(0, acVersion);
    m_pOverlay->ExecuteAsync("setVersion", pArguments);
}

void OverlayService::SendSystemMessage(const std::string& acMessage)
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kSystemMessage);
    pArguments->SetString(1, acMessage);

    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::SetPlayerHealthPercentage(uint32_t aFormId) const noexcept
{
    Actor* pActor = Cast<Actor>(TESForm::GetById(aFormId));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto view = m_world.view<FormIdComponent, PlayerComponent>();
    auto entityIt = std::find_if(view.begin(), view.end(), [view, aFormId](auto aEntity) { return view.get<FormIdComponent>(aEntity).Id == aFormId; });

    if (entityIt == view.end())
    {
        spdlog::error("{}: cannot find player entity for form id {:X}", __FUNCTION__, aFormId);
        return;
    }

    const auto& playerComponent = view.get<PlayerComponent>(*entityIt);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::SignalBrowserReady() noexcept
{
    // CEF thread: publish only an atomic signal. The game thread applies
    // it without depending on the runner queue.
    m_browserReadySignal.store(true, std::memory_order_release);
    BSGraphics::WakeMainMenuPump();
}

void OverlayService::ProcessBrowserReady() noexcept
{
    if (m_pOverlay && m_browserReadySignal.exchange(false, std::memory_order_acq_rel))
        OnBrowserReady();
}

void OverlayService::OnBrowserReady() noexcept
{
    if (!m_pOverlay || m_browserReady) return;
    m_browserReady = true;
    spdlog::info("Skyrim Together frontend ready");
    Initialize();
    SetVersion(BUILD_COMMIT);
    RefreshMenuPresentation();
    auto lobby = CefListValue::Create();
    lobby->SetBool(0, m_transport.IsInLobby());
    m_pOverlay->ExecuteAsync("serverLobby", lobby);
    if (m_transport.IsOnline())
    {
        m_pOverlay->ExecuteAsync("connect");
        auto player = CefListValue::Create();
        player->SetInt(0, m_transport.GetLocalPlayerId());
        m_pOverlay->ExecuteAsync("setLocalPlayerId", player);
    }
    m_pOverlay->ExecuteAsync(m_active ? "activate" : "deactivate");
    PublishDialogueFocus(true);
}

void OverlayService::RefreshMenuPresentation() noexcept
{
    if (!m_pOverlay)
        return;
    auto args = CefListValue::Create();
    args->SetBool(0, m_mainMenu);
    m_pOverlay->ExecuteAsync("mainMenu", args);
    m_pOverlay->ExecuteAsync(m_inGame ? "enterGame" : "exitGame");
    CharacterCheckpoints::PublishState();
}

void OverlayService::OnServerLobbyEvent(const ServerLobbyEvent& acEvent) noexcept
{
    if (!m_pOverlay)
        return;
    auto args = CefListValue::Create();
    args->SetBool(0, acEvent.Authenticated);
    m_pOverlay->ExecuteAsync("serverLobby", args);
    if (!acEvent.Authenticated)
        m_pOverlay->ExecuteAsync("disconnect");
}

void OverlayService::UpdateMainMenu() noexcept
{
    if (!m_pOverlay)
        return;
    auto* pUI = UI::Get();
    const bool mainMenu = pUI && pUI->IsMainMenuOpen();
    if (m_mainMenu != mainMenu)
    {
        m_mainMenu = mainMenu;
        m_menuOpenRequested = false;
        m_settingsOpenRequested = false;
        m_browserWaitLogged = false;
        m_mainMenuSince = std::chrono::steady_clock::now();
        spdlog::info("Skyrim Together main menu: {}", m_mainMenu);
        auto args = CefListValue::Create();
        args->SetBool(0, m_mainMenu);
        m_pOverlay->ExecuteAsync("mainMenu", args);
        if (!m_mainMenu && !m_inGame && m_active)
        {
            SetActive(false);
            TiltedPhoques::DInputHook::Get().SetEnabled(false);
            if (const auto client = m_pOverlay->GetClient())
                if (const auto renderer = client->GetOverlayRenderHandler())
                    renderer->SetCursorVisible(false);
        }
    }
    MainMenuBridge::Tick(m_mainMenu);
    if (m_mainMenu)
    {
        if (MainMenuBridge::ConsumePlayRequest())
        {
            m_menuOpenRequested = true;
            spdlog::info("Native Play request queued");
        }
        if (MainMenuBridge::ConsumeSettingsRequest())
            m_settingsOpenRequested = true;
        if (m_browserReady)
        {
            const HWND window = m_world.ctx().at<RenderSystemD3D11>().GetWindow();
            if ((m_menuOpenRequested || m_settingsOpenRequested) && InputService::SetOverlayActive(*this, true))
            {
                if (m_settingsOpenRequested)
                    m_pOverlay->ExecuteAsync("openSettings");
                m_menuOpenRequested = false;
                m_settingsOpenRequested = false;
            }
            InputService::PollMainMenuHotkey(*this, window);
        }
        else if (!m_browserWaitLogged && std::chrono::steady_clock::now() - m_mainMenuSince >= std::chrono::seconds(5))
        {
            m_browserWaitLogged = true;
            spdlog::warn("Main menu is open but frontend readiness was not applied; compare uiReady receipt and frontend-ready logs");
        }
    }
    else
        InputService::PollMainMenuHotkey(*this, nullptr);
}

void OverlayService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_pOverlay) return;
    UpdateMainMenu();
    if (m_mainMenu && !m_transport.IsOnline()) return;
    RefreshDialogueSession();
    PublishDialogueFocus();
    PublishDialogueOptions();
    if (m_transport.IsOnline())
        DialoguePortraits::Update(m_world, m_pOverlay.get(), m_transport.GetLocalPlayerId());
    RunDebugDataUpdates();
    RunPlayerHealthUpdates();
}

void OverlayService::OnConnectedEvent(const ConnectedEvent& acEvent) noexcept
{
    DialoguePortraits::Reset();
    InputService::SetWorldCursorSuppressed(true);
    if (!m_pOverlay) return;
    // World entry is complete: release lobby input and the overlay's cursor.
    if (!InputService::SetOverlayActive(*this, false))
    {
        SetActive(false);
        TiltedPhoques::DInputHook::Get().SetEnabled(false);
        if (const auto client = m_pOverlay->GetClient())
            if (const auto renderer = client->GetOverlayRenderHandler())
                renderer->SetCursorVisible(false);
    }
    m_pOverlay->ExecuteAsync("connect");

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acEvent.PlayerId);
    m_pOverlay->ExecuteAsync("setLocalPlayerId", pArguments);
}

void OverlayService::OnDisconnectedEvent(const DisconnectedEvent&) noexcept
{
    InputService::SetWorldCursorSuppressed(false);
    CloseDialogueSession();
    PublishDialogueFocus(true);
    DialoguePortraits::Reset();
    m_pOverlay->ExecuteAsync("disconnect");
}

void OverlayService::OnWaitingFor3DRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    const auto* pPlayerComponent = m_world.try_get<PlayerComponent>(aEntity);
    if (!pPlayerComponent)
        return;

    const auto& formIdComponent = m_world.get<FormIdComponent>(aEntity);

    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
    if (!pActor)
    {
        spdlog::error("{}: cannot find actor for form id {:X}", __FUNCTION__, formIdComponent.Id);
        return;
    }

    float percentage = CalculateHealthPercentage(pActor);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, pPlayerComponent->Id);
    pArguments->SetInt(1, static_cast<int>(percentage));

    m_pOverlay->ExecuteAsync("setPlayer3dLoaded", pArguments);
}

void OverlayService::OnPlayerComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) const noexcept
{
    const auto& playerComponent = m_world.get<PlayerComponent>(aEntity);

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, playerComponent.Id);

    m_pOverlay->ExecuteAsync("setPlayer3dUnloaded", pArguments);
}

void OverlayService::OnTravelInvitation(const NotifyTravelInvitation& acMessage) noexcept
{
    if (!m_pOverlay)
        return;
    auto args = CefListValue::Create();
    args->SetDouble(0, acMessage.Id);
    args->SetInt(1, acMessage.RemainingMs);
    args->SetInt(2, acMessage.ParticipantCount);
    args->SetBool(3, acMessage.CanCancel);
    args->SetBool(4, acMessage.Accepted);
    args->SetString(5, acMessage.LeaderName.c_str());
    if (acMessage.RemainingMs)
    {
        auto destination = GetCellName({}, acMessage.CellId);
        if (destination == "UNKNOWN" && acMessage.WorldSpaceId)
            destination = GetCellName(acMessage.WorldSpaceId, {});
        args->SetString(6, destination.c_str());
    }
    else
        args->SetString(6, "");
    m_pOverlay->ExecuteAsync("travelInvitation", args);
}

void OverlayService::OnChatMessageReceived(const NotifyChatMessageBroadcast& acMessage) noexcept
{
    if (!m_pOverlay)
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, (int)acMessage.MessageType);
    pArguments->SetString(1, acMessage.ChatMessage.c_str());
    pArguments->SetString(2, acMessage.PlayerName.c_str());

    m_pOverlay->ExecuteAsync("message", pArguments);
}

bool OverlayService::SendDialogueAction(uint8_t aAction, const char* apText) noexcept
{
    if (!m_transport.IsOnline() || !m_dialogueToken)
        return false;
    PlayerDialogueRequest request{};
    request.Action = aAction;
    request.ActorId = m_dialogueActorId;
    request.Token = m_dialogueToken;
    if (apText)
        request.Text = apText;
    return m_transport.Send(request);
}

void OverlayService::CloseDialogueSession() noexcept
{
    // Invalidate the native selection grant locally before releasing it to someone else.
    m_dialogueGranted = false;
    m_dialogueCanChoose = false;
    SendDialogueAction(PlayerDialogueRequest::Leave);
    m_dialogueActorId = 0;
    m_dialogueToken = 0;
    m_dialogueSpeakerHandle = 0;
    m_dialoguePanelRevision = 0;
    m_dialogueYielded = false;
    m_dialogueOptionsPublished = false;
    m_lastDialogueOptions.clear();
}

void OverlayService::RefreshDialogueSession() noexcept
{
    if (!m_transport.IsOnline())
        return;

    auto* pManager = MenuTopicManager::Get();
    if (!pManager || !pManager->menuOpen || !pManager->speaker)
    {
        if (m_dialogueToken)
            CloseDialogueSession();
        return;
    }

    const uint32_t cHandle = pManager->speaker.handle.iBits;
    auto* pSpeaker = TESObjectREFR::GetByHandle(cHandle);

    // Guard dialogue can apply personal crime and custody consequences.
    const auto* pActor = Cast<Actor>(pSpeaker);
    if (pSpeaker && (!pActor || pActor->IsGuard()))
    {
        if (m_dialogueToken)
            CloseDialogueSession();
        return;
    }

    if (m_dialogueToken && m_dialogueSpeakerHandle == cHandle)
        return;

    if (m_dialogueToken)
        CloseDialogueSession();

    if (!pSpeaker)
        return;

    // Use the replicated actor entity, not a local runtime FormID, so dynamic NPCs
    // and clients with different load orders refer to the same conversation.
    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id != pSpeaker->formID)
            continue;
        const auto serverId = Utils::GetServerId(entity);
        if (!serverId)
            continue;

        m_dialogueActorId = *serverId;
        m_dialogueSpeakerHandle = cHandle;
        if (++m_nextDialogueToken == 0)
            ++m_nextDialogueToken;
        m_dialogueToken = m_nextDialogueToken;
        m_dialogueGranted = false;
        m_dialogueCanChoose = false;
        if (!SendDialogueAction(PlayerDialogueRequest::Join))
            CloseDialogueSession();
        return;
    }
}

void OverlayService::PublishDialogueFocus(bool aForce) noexcept
{
    const auto* manager = MenuTopicManager::Get();
    const bool open = m_transport.IsOnline() && manager && manager->menuOpen;
    const uint32_t actor = open && m_dialogueToken ? m_dialogueActorId : 0;
    if (!aForce && open == m_dialogueFocusOpen && actor == m_dialogueFocusActor) return;
    m_dialogueFocusOpen = open;
    m_dialogueFocusActor = actor;
    if (!m_pOverlay) return;
    // Passive UI only; the native menu retains all input and topic selection.
    auto args = CefListValue::Create();
    args->SetBool(0, open);
    args->SetDouble(1, actor);
    m_pOverlay->ExecuteAsync("dialogueFocus", args);
}

void OverlayService::PublishDialogueOptions() noexcept
{
    if (!m_transport.IsOnline() || !m_dialogueGranted || !m_dialogueCanChoose ||
        std::chrono::steady_clock::now() < m_dialoguePublishAfter)
        return;
    const auto* pManager = MenuTopicManager::Get();
    if (!pManager || !pManager->menuOpen || pManager->speaker.handle.iBits != m_dialogueSpeakerHandle)
        return;

    std::vector<String> options;
    if (pManager->pOptions)
    {
        for (const auto* pOption : *pManager->pOptions)
        {
            if (options.size() >= 64)
                break;
            options.emplace_back(pOption && pOption->text ? pOption->text : "");
        }
    }
    if (m_dialogueOptionsPublished && options == m_lastDialogueOptions)
        return;
    PlayerDialogueRequest request{};
    request.Action = PlayerDialogueRequest::PublishOptions;
    request.ActorId = m_dialogueActorId;
    request.Token = m_dialogueToken;
    request.Options = options;
    if (m_transport.Send(request))
    {
        m_lastDialogueOptions = std::move(options);
        m_dialogueOptionsPublished = true;
    }
}

void OverlayService::ManageDialogue(uint32_t aActorId, uint32_t aRevision, uint32_t aAction) noexcept
{
    if (aAction == 3 || aAction == 4)
    {
        PlayerDialogueRequest request{};
        request.Action = aAction == 3 ? PlayerDialogueRequest::Listen : PlayerDialogueRequest::StopListening;
        request.ActorId = aActorId;
        request.Revision = aRevision;
        m_transport.Send(request);
        return;
    }

    RefreshDialogueSession();
    if (!m_dialogueGranted || !m_dialogueCanChoose || aActorId != m_dialogueActorId ||
        !aRevision || aRevision != m_dialoguePanelRevision)
        return;

    if (aAction == 0)
    {
        // Revoke the local grant before sending Leave. Keep the token until
        // the native menu closes so it cannot automatically rejoin.
        m_dialogueYielded = true;
        m_dialogueCanChoose = false;
        if (!SendDialogueAction(PlayerDialogueRequest::Leave))
        {
            m_dialogueYielded = false;
            m_dialogueCanChoose = true;
        }
        return;
    }

    if (aAction == 1 || aAction == 2)
    {
        PlayerDialogueRequest request{};
        request.Action = PlayerDialogueRequest::SetPrivacy;
        request.ActorId = aActorId;
        request.Token = m_dialogueToken;
        request.Revision = aRevision;
        request.Index = aAction == 1 ? 1 : 0;
        m_transport.Send(request);
    }
}

uint32_t OverlayService::GetDialogueGrant(uint32_t aActorId) const noexcept
{
    return m_dialogueGranted && m_dialogueCanChoose && m_dialogueActorId == aActorId ? m_dialogueToken : 0;
}

bool OverlayService::CanSelectDialogue(const char* apText) noexcept
{
    if (!m_transport.IsOnline())
        return true;

    RefreshDialogueSession();

    // Talking activators and guards use local native choices. Keep waiting
    // for a grant when the speaker cannot be resolved yet.
    const auto* pManager = MenuTopicManager::Get();
    if (pManager && pManager->menuOpen && pManager->speaker)
    {
        auto* pSpeaker = TESObjectREFR::GetByHandle(pManager->speaker.handle.iBits);
        const auto* pActor = Cast<Actor>(pSpeaker);
        if (pSpeaker && (!pActor || pActor->IsGuard()))
            return true;
    }

    if (m_dialogueYielded)
    {
        Utils::ShowHudMessage("You passed control. Close and reopen dialogue to rejoin.");
        return false;
    }
    if (!m_dialogueGranted)
    {
        Utils::ShowHudMessage("Waiting for shared dialogue control. Reopen the conversation if it cannot connect.");
        return false;
    }

    if (m_dialogueCanChoose)
    {
        if (SendDialogueAction(PlayerDialogueRequest::Choose, apText))
        {
            m_dialogueOptionsPublished = false;
            m_dialoguePublishAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            return true;
        }
        Utils::ShowHudMessage("Unable to send dialogue choice.");
        return false;
    }

    if (!apText || !*apText)
        return false;
    if (SendDialogueAction(PlayerDialogueRequest::Suggest, apText))
        Utils::ShowHudMessage("Dialogue suggestion sent to the conversation owner.");
    else
        Utils::ShowHudMessage("Unable to send dialogue suggestion.");
    return false;
}

void OverlayService::OnPlayerDialogue(const NotifyPlayerDialogue& acMessage) noexcept
{
    if (acMessage.Kind == NotifyPlayerDialogue::Panel || acMessage.Kind == NotifyPlayerDialogue::PanelClosed)
    {
        DialoguePortraits::Observe(acMessage);
        if (acMessage.ActorId == m_dialogueActorId)
            m_dialoguePanelRevision = acMessage.CanChoose ? acMessage.Revision : 0;
        if (!m_pOverlay)
            return;
        auto pArguments = CefListValue::Create();
        pArguments->SetDouble(0, acMessage.ActorId);
        pArguments->SetBool(1, acMessage.Kind == NotifyPlayerDialogue::Panel);
        pArguments->SetString(2, acMessage.Name.c_str());
        pArguments->SetBool(3, acMessage.CanChoose);
        pArguments->SetDouble(4, acMessage.Revision);
        const TESObjectREFR* pSpeaker = Utils::GetByServerId<Actor>(acMessage.ActorId);
        if (!pSpeaker && acMessage.SpeakerReference)
        {
            const uint32_t cLocalId = m_world.GetModSystem().GetGameId(acMessage.SpeakerReference);
            if (cLocalId)
                pSpeaker = Cast<TESObjectREFR>(TESForm::GetById(cLocalId));
        }
        const char* pName = pSpeaker && pSpeaker->baseForm ? pSpeaker->baseForm->GetName() : nullptr;
        pArguments->SetString(5, pName && *pName ? pName : "NPC");
        auto options = CefListValue::Create();
        auto votes = CefListValue::Create();
        for (size_t i = 0; i < acMessage.Options.size(); ++i)
        {
            options->SetString(i, acMessage.Options[i].c_str());
            votes->SetInt(i, i < acMessage.Votes.size() ? static_cast<int>(acMessage.Votes[i]) : 0);
        }
        pArguments->SetList(6, options);
        pArguments->SetList(7, votes);
        pArguments->SetInt(8, acMessage.Selected == UINT32_MAX ? -1 : static_cast<int>(acMessage.Selected));
        pArguments->SetBool(9, acMessage.CanVote);
        pArguments->SetBool(10, acMessage.CanPass);
        pArguments->SetBool(11, acMessage.Private);
        auto voterIds = CefListValue::Create();
        auto voterOptions = CefListValue::Create();
        for (size_t i = 0; i < acMessage.VoterChoices.size(); ++i)
        {
            voterIds->SetDouble(i, acMessage.VoterChoices[i].first);
            voterOptions->SetInt(i, static_cast<int>(acMessage.VoterChoices[i].second));
        }
        pArguments->SetList(12, voterIds);
        pArguments->SetList(13, voterOptions);
        pArguments->SetBool(14, acMessage.Remote);
        pArguments->SetBool(15, acMessage.Listening);
        pArguments->SetString(16, acMessage.Text.c_str());
        m_pOverlay->ExecuteAsync("sharedDialogue", pArguments);
        return;
    }

    if (m_dialogueYielded || !m_dialogueToken || acMessage.Token != m_dialogueToken || acMessage.ActorId != m_dialogueActorId)
        return;

    if (acMessage.Kind == NotifyPlayerDialogue::State)
    {
        const bool cChanged = !m_dialogueGranted || m_dialogueCanChoose != acMessage.CanChoose;
        m_dialogueGranted = true;
        m_dialogueCanChoose = acMessage.CanChoose;
        if (cChanged)
            m_dialogueOptionsPublished = false;
        if (cChanged)
        {
            if (m_dialogueCanChoose)
                Utils::ShowHudMessage("You control this conversation.");
            else
            {
                const String message = acMessage.Name + " controls this conversation. Choose a response to suggest it.";
                Utils::ShowHudMessage(message);
            }
        }
        return;
    }

    if (acMessage.Kind == NotifyPlayerDialogue::Denied)
    {
        m_dialogueGranted = false;
        m_dialogueCanChoose = false;
        Utils::ShowHudMessage("Shared dialogue unavailable. Close and reopen the conversation.");
        return;
    }

    // Suggestions are displayed alongside the dialogue choices instead of duplicating them as HUD toasts.

    if (!m_pOverlay || (acMessage.Kind != NotifyPlayerDialogue::Choice && acMessage.Kind != NotifyPlayerDialogue::Suggestion))
        return;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, kPlayerDialogue);
    const std::string text = (acMessage.Kind == NotifyPlayerDialogue::Suggestion ? "[Suggestion] " : "") +
        std::string(acMessage.Text.c_str());
    pArguments->SetString(1, text);
    pArguments->SetString(2, acMessage.Name.c_str());
    m_pOverlay->ExecuteAsync("message", pArguments);
}

void OverlayService::OnConnectionError(const ConnectionErrorEvent& acConnectedEvent) const noexcept
{
    auto pArgs = CefListValue::Create();
    pArgs->SetString(0, acConnectedEvent.ErrorDetail.c_str());
    m_pOverlay->ExecuteAsync("triggerError", pArgs);
}

void OverlayService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    pArguments->SetInt(2, acMessage.Level);

    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(3, cellName.c_str());

    m_pOverlay->ExecuteAsync("playerConnected", pArguments);
    if (acMessage.PlayerId == m_transport.GetLocalPlayerId())
    {
        auto loaded = CefListValue::Create();
        loaded->SetInt(0, acMessage.PlayerId);
        loaded->SetInt(1, static_cast<int>(CalculateHealthPercentage(PlayerCharacter::Get())));
        m_pOverlay->ExecuteAsync("setPlayer3dLoaded", loaded);
    }
}

void OverlayService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    DialoguePortraits::RemovePlayer(acMessage.PlayerId);
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetString(1, acMessage.Username.c_str());
    m_pOverlay->ExecuteAsync("playerDisconnected", pArguments);
}

void OverlayService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetInt(1, acMessage.NewLevel);
    m_pOverlay->ExecuteAsync("setLevel", pArguments);
}

void OverlayService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) const noexcept
{
    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    String cellName = GetCellName(acMessage.WorldSpaceId, acMessage.CellId);
    pArguments->SetString(1, cellName.c_str());
    m_pOverlay->ExecuteAsync("setCell", pArguments);
}

void OverlayService::OnNotifyPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    const float percentage = acMessage.Percentage >= 0.f ? acMessage.Percentage : 0.f;

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, acMessage.PlayerId);
    pArguments->SetDouble(1, static_cast<double>(percentage));
    m_pOverlay->ExecuteAsync("setHealth", pArguments);
}

void OverlayService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    // The campaign roster follows immediately in NotifyPartyInfo.
}

void OverlayService::OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept
{
    m_world.GetOverlayService().GetOverlayApp()->ExecuteAsync("partyLeft");
}

void OverlayService::RunDebugDataUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto internalStats = m_transport.GetStatistics();
    auto steamStats = m_transport.GetConnectionStatus();

    auto pArguments = CefListValue::Create();
    pArguments->SetInt(0, steamStats.m_flOutPacketsPerSec);
    pArguments->SetInt(1, steamStats.m_flInPacketsPerSec);
    pArguments->SetInt(2, steamStats.m_nPing);
    pArguments->SetInt(3, 0);
    pArguments->SetInt(4, internalStats.SentBytes);
    pArguments->SetInt(5, internalStats.RecvBytes);

    m_pOverlay->ExecuteAsync("debugData", pArguments);
}

// TODO(cosideci): this whole thing is a really hacky solution to
// health sync code being somewhat broken for players.
void OverlayService::RunPlayerHealthUpdates() noexcept
{
    if (!m_transport.IsConnected() || !m_world.GetPartyService().IsInParty())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    static float s_previousPercentage = -1.f;
    static uint32_t s_previousPlayer = 0;
    const uint32_t localId = m_transport.GetLocalPlayerId();
    if (localId != s_previousPlayer)
    {
        s_previousPlayer = localId;
        s_previousPercentage = -1.f;
    }

    const float newPercentage = CalculateHealthPercentage(PlayerCharacter::Get());
    if (newPercentage == s_previousPercentage)
        return;

    s_previousPercentage = newPercentage;
    auto local = CefListValue::Create();
    local->SetInt(0, localId);
    local->SetDouble(1, static_cast<double>(newPercentage));
    m_pOverlay->ExecuteAsync("setHealth", local);

    RequestPlayerHealthUpdate request{};
    request.Percentage = newPercentage;

    m_transport.Send(request);
}
