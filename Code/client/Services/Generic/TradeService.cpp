#include <TiltedOnlinePCH.h>
#include <Services/TradeService.h>
#include <World.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Camera/PlayerCamera.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <charconv>
#include <cmath>
#include <array>
#include <string_view>
#include <filesystem>

namespace
{
std::string JsonString(std::string_view s)
{
    std::string out = "\"";
    for (const unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
        else out += static_cast<char>(c);
    }
    return out + '"';
}
bool InSession(const NotifyTrade& s) noexcept
{
    return s.Session && (s.Phase == Trade::Pending || s.Phase == Trade::Open);
}
}
TradeService::TradeService(World& world, entt::dispatcher& dispatcher) noexcept : m_world(world)
{
    m_notify = dispatcher.sink<NotifyTrade>().connect<&TradeService::OnNotify>(this);
    m_disconnect = dispatcher.sink<DisconnectedEvent>().connect<&TradeService::OnDisconnect>(this);
    m_update = dispatcher.sink<UpdateEvent>().connect<&TradeService::OnUpdate>(this);
}
bool TradeService::EnsureView() noexcept
{
    using namespace MAGELIGHT_API;
    if (!m_api) m_api = RequestApi4();
    if (!m_api) return false;
    if (!m_mod)
    {
        ModDesc desc{};
        desc.size = sizeof(desc);
        desc.modId = "SkyrimTogetherTrade";
        desc.displayName = "Skyrim Together Trading";
        desc.modVersion = 10000;
        desc.minHostVersion = 10903;
        desc.callbackThread = CallbackThread::GameThread;
        desc.onEvent = &OnHostEvent;
        ModId mod{};
        if (m_api->RegisterMod(&desc, &mod) != Result::Ok || !mod) return false;
        m_mod = mod;
    }
    if (m_view && m_api->IsViewValid(m_view)) return true;
    wchar_t exe[32768]{};
    const auto length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (!length || length >= std::size(exe)) return false;
    const auto path = std::filesystem::path(exe).parent_path() /
        L"Data/Magelight/SkyrimTogetherTrade/views/trade/index.html";
    const auto utf8 = path.u8string();
    const std::string page(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    ViewDesc desc{};
    desc.size = sizeof(desc);
    desc.name = "trade";
    desc.htmlPath = page.c_str();
    desc.fullscreen = true;
    desc.layer = Layer::Panel;
    desc.uiScale = 1.f;
    m_ready = false;
    m_focusRequested = false;
    ViewId view{};
    if (m_api->CreateViewEx(m_mod, &desc, &view) != Result::Ok || !view)
    {
        m_view = 0;
        return false;
    }
    m_view = view;
    return true;
}
void TradeService::OnHostEvent(const MAGELIGHT_API::EventData* event, void*) noexcept
{
    if (!event || !entt::locator<World>::has_value()) return;
    const auto kind = event->type;
    const auto view = event->view;
    World::Get().QueueCheckpointTask([kind,view]()
    {
        World::Get().ctx().at<TradeService>().HandleHostEvent(kind,view);
    });
}
void TradeService::HandleHostEvent(MAGELIGHT_API::Event kind, MAGELIGHT_API::ViewId view) noexcept
{
    using namespace MAGELIGHT_API;
    if (!m_api) return;
    if (kind == Event::RenderDead)
    {
        CancelAndHide();
        m_ready = false;
        return;
    }
    if (!m_view || m_view != view) return;
    if (kind == Event::ViewDomReady)
    {
        if (m_api->RegisterJSListenerEx(view,"tradeCommand",&OnCommand,nullptr) != Result::Ok)
        {
            CancelAndHide();
            return;
        }
        m_ready = true;
        if (m_show) Present();
    }
    else if (kind == Event::UIModeEntered)
    {
        if (!m_show) Hide();
    }
    else if (kind == Event::UIModeExited || kind == Event::UIModeRefused)
    {
        // Escape is handled by Magelight itself as well as by the page.
        // Cancel the session on any host-driven close; never steal another mod's focus.
        if (m_show) CancelAndHide();
        m_focusRequested = false;
    }
    else if (kind == Event::ViewLoadFailed || kind == Event::ViewDestroyed)
    {
        m_ready = false;
        CancelAndHide();
        if (kind == Event::ViewDestroyed) m_view = 0;
        m_world.GetOverlayService().SendSystemMessage("Magelight could not load the trade window.");
    }
}
void TradeService::OnCommand(MAGELIGHT_API::ViewId view, const char* text, void*) noexcept
{
    if (!text || !entt::locator<World>::has_value()) return;
    const size_t length = strnlen_s(text,128);
    if (!length || length >= 128) return;
    // Copy the argument before returning to the page thread. Together's
    // game-window task queue is drained independently of the host renderer.
    World::Get().QueueCheckpointTask([view, command = std::string(text,length)]()
    {
        auto& self = World::Get().ctx().at<TradeService>();
        if (self.m_view == view && self.m_show) self.Command(command);
    });
}
void TradeService::Command(const std::string& text) noexcept
{
    if (text == "hide")
    {
        if (!InSession(m_state)) Hide();
        return;
    }
    std::array<uint32_t,5> values{};
    std::string_view rest(text);
    for (size_t i = 0; i < values.size(); ++i)
    {
        const auto split = rest.find(':');
        if ((i == values.size()-1) != (split == rest.npos)) return;
        const auto field = rest.substr(0,split);
        const auto result = std::from_chars(field.data(),field.data()+field.size(),values[i]);
        if (result.ec != std::errc{} || result.ptr != field.data()+field.size()) return;
        if (split != rest.npos) rest.remove_prefix(split+1);
    }
    if (!m_world.GetTransport().IsOnline() || !InSession(m_state) ||
        values[0] < Trade::Accept || values[0] > Trade::Cancel ||
        values[1] != m_state.Session || values[2] != m_state.Revision) return;
    RequestTrade request{};
    request.Action = static_cast<uint8_t>(values[0]);
    request.Session = values[1]; request.Revision = values[2];
    request.Index = values[3]; request.Quantity = values[4];
    if (request.Action == Trade::Cancel) { CancelAndHide(); return; }
    m_world.GetTransport().Send(request);
}
void TradeService::OnUpdate(const UpdateEvent&) noexcept
{
    if (m_world.IsGameThread()) { Tick(); return; }
    if (m_queued.exchange(true)) return;
    m_world.QueueCheckpointTask([this]() { Tick(); m_queued.store(false); });
}
void TradeService::Tick() noexcept
{
    if (m_show && m_openDeadline != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= m_openDeadline &&
        (!m_ready || !m_api || m_api->GetUIModeOwner() != m_mod))
    {
        CancelAndHide();
        m_world.GetOverlayService().SendSystemMessage("The Magelight trade window could not open.");
    }
    const bool down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('E') & 0x8000);
    const bool pressed = down && !m_keyDown;
    m_keyDown = down;
    if (!pressed || !m_world.GetTransport().IsOnline() || !m_world.GetOverlayService().GetInGame() ||
        m_world.GetOverlayService().GetActive() || GetForegroundWindow() != GetActiveWindow()) return;
    auto* ui = UI::Get();
    if (!ui || ui->numPausesGame || ui->IsUnpausedMenuOpen() || !ui->GetMenuOpen("HUD Menu")) return;
    if (const auto* api = MAGELIGHT_API::RequestApi4(); api && api->IsUIModeActive()) return;
    InviteTarget();
}
void TradeService::InviteTarget() noexcept
{
    if (InSession(m_state) || m_show) return;
    auto* player = PlayerCharacter::Get();
    auto* camera = PlayerCamera::Get();
    if (!player || !camera) return;
    uint32_t target{};
    float best = .065f * .065f;
    const auto view = m_world.view<PlayerComponent, RemoteComponent, FormIdComponent>(entt::exclude<WaitingFor3D>);
    for (auto entity : view)
    {
        const auto id = view.get<PlayerComponent>(entity).Id;
        if (!id || id == m_world.GetTransport().GetLocalPlayerId()) continue;
        auto* actor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!actor || actor->parentCell != player->parentCell) continue;
        const glm::vec3 delta = actor->position - player->position;
        if (glm::dot(delta,delta) > 400.f*400.f) continue;
        auto point = actor->position; point.z += 70.f;
        NiPoint3 screen{};
        if (!camera->WorldPtToScreenPt3(point,screen) || screen.z < 0.f) continue;
        const float x = screen.x-.5f, y = screen.y-.5f;
        const float score = x*x + y*y;
        if (score < best) { best=score; target=id; }
    }
    if (!target) return;
    if (!EnsureView())
    {
        m_world.GetOverlayService().SendSystemMessage("Trading requires Magelight UI 1.9.3 or newer.");
        return;
    }
    RequestTrade request{};
    request.Action = Trade::Invite;
    request.Target = target;
    m_world.GetTransport().Send(request);
}
void TradeService::OnNotify(const NotifyTrade& message) noexcept
{
    if (!m_world.IsGameThread())
    {
        const auto player = m_world.GetTransport().GetLocalPlayerId();
        m_world.QueueCheckpointTask([this,message,player]()
        {
            if (player && m_world.GetTransport().IsOnline() && m_world.GetTransport().GetLocalPlayerId()==player)
                OnNotify(message);
        });
        return;
    }
    if (message.Session && message.Session == m_dismissedSession) return;
    if (!EnsureView())
    {
        if (message.Session && (message.Phase == Trade::Pending || message.Phase == Trade::Open))
        {
            RequestTrade cancel{};
            cancel.Action=Trade::Cancel; cancel.Session=message.Session; cancel.Revision=message.Revision;
            m_world.GetTransport().Send(cancel);
        }
        m_world.GetOverlayService().SendSystemMessage("Trade unavailable: install Magelight UI 1.9.3 or newer.");
        return;
    }
    if (!m_show)
    {
        m_openDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        m_focusRequested = false;
    }
    m_state=message; m_show=true;
    m_api->ShowView(m_view,true);
    Present();
}
void TradeService::Present() noexcept
{
    if (!m_api || !m_view || !m_ready || !m_show) return;
    const auto rows = [&](const auto& source)
    {
        std::string json="[";
        for (size_t i=0;i<source.size();++i)
        {
            const auto& row=source[i];
            const auto formId=m_world.GetModSystem().GetGameId(row.Item);
            const auto* form=TESForm::GetById(formId);
            const char* name=form ? form->GetName() : nullptr;
            const auto label=name && *name ? std::string(name) : fmt::format("Item {:08X}",formId);
            if (i) json+=",";
            json+=fmt::format("{{\"index\":{},\"name\":{},\"available\":{},\"offered\":{},\"improved\":{},\"enchanted\":{}}}",
                i,JsonString(label),row.Available,row.Offered,row.Improved,row.Enchanted);
        }
        return json+"]";
    };
    std::string partner=fmt::format("Player {}",m_state.Partner);
    const auto view=m_world.view<PlayerComponent,FormIdComponent>();
    for (auto entity:view)
    {
        if(view.get<PlayerComponent>(entity).Id!=m_state.Partner) continue;
        if(auto* actor=Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id)))
            if(const char* name=actor->GetName();name && *name) partner=name;
        break;
    }
    const auto json=fmt::format("{{\"phase\":{},\"session\":{},\"revision\":{},\"partner\":{},\"incoming\":{},\"mineConfirmed\":{},\"otherConfirmed\":{},\"mine\":{},\"other\":{}}}",
        m_state.Phase,m_state.Session,m_state.Revision,JsonString(partner),m_state.Incoming,
        m_state.MineConfirmed,m_state.OtherConfirmed,rows(m_state.Mine),rows(m_state.Other));
    m_api->ShowView(m_view,true);
    m_api->InteropCall(m_view,"updateTrade",json.c_str());
    if (m_api->GetUIModeOwner() != m_mod && !m_focusRequested)
    {
        m_focusRequested = true;
        if (m_api->RequestUIMode(m_view,MAGELIGHT_API::kUIModeFlagNone) != MAGELIGHT_API::Result::Ok)
        {
            CancelAndHide();
            m_world.GetOverlayService().SendSystemMessage("Trade could not take focus. Close the other menu and try again.");
        }
    }
}
void TradeService::CancelAndHide() noexcept
{
    if (InSession(m_state) && m_world.GetTransport().IsOnline())
    {
        RequestTrade cancel{};
        cancel.Action = Trade::Cancel;
        cancel.Session = m_state.Session;
        cancel.Revision = m_state.Revision;
        m_dismissedSession = m_state.Session;
        m_world.GetTransport().Send(cancel);
    }
    m_state = NotifyTrade{};
    Hide();
}
void TradeService::Hide() noexcept
{
    m_show = false;
    m_focusRequested = false;
    m_openDeadline = {};
    if (!m_api || !m_mod) return;
    m_api->CancelUIModeRequest(m_mod);
    if (m_api->GetUIModeOwner() == m_mod) m_api->ReleaseUIMode(m_mod);
    if (m_view && m_api->IsViewValid(m_view)) m_api->ShowView(m_view,false);
}
void TradeService::OnDisconnect(const DisconnectedEvent&) noexcept
{
    m_world.QueueCheckpointTask([this]() { m_state=NotifyTrade{}; m_dismissedSession=0; Hide(); });
}
