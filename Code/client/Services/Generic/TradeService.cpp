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
    if (!m_api)
        m_api = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
    if (!m_api) return false;
    if (m_view && m_api->IsValid(m_view)) return true;
    m_ready = false;
    m_view = m_api->CreateView("SkyrimTogetherTrade/index.html", &DomReady);
    if (!m_view) return false;
    m_api->Hide(m_view);
    return true;
}
void TradeService::DomReady(PrismaView view) noexcept
{
    if (!entt::locator<World>::has_value()) return;
    World::Get().QueueCheckpointTask([view]()
    {
        auto& self = World::Get().ctx().at<TradeService>();
        if (self.m_view != view || !self.m_api) return;
        self.m_api->RegisterJSListener(view, "tradeCommand", &OnCommand);
        self.m_ready = true;
        if (self.m_show) self.Present();
    });
}
void TradeService::OnCommand(const char* text) noexcept
{
    if (!text || !entt::locator<World>::has_value()) return;
    const size_t length = strnlen_s(text,128);
    if (!length || length >= 128) return;
    World::Get().QueueCheckpointTask([command = std::string(text,length)]()
    {
        World::Get().ctx().at<TradeService>().Command(command);
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
    const bool down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('E') & 0x8000);
    const bool pressed = down && !m_keyDown;
    m_keyDown = down;
    if (!pressed || !m_world.GetTransport().IsOnline() || !m_world.GetOverlayService().GetInGame() ||
        m_world.GetOverlayService().GetActive() || GetForegroundWindow() != GetActiveWindow()) return;
    auto* ui = UI::Get();
    if (!ui || ui->numPausesGame || ui->IsUnpausedMenuOpen() || !ui->GetMenuOpen("HUD Menu")) return;
    if (m_api && m_api->HasAnyActiveFocus()) return;
    InviteTarget();
}
void TradeService::InviteTarget() noexcept
{
    if (InSession(m_state)) return;
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
        m_world.GetOverlayService().SendSystemMessage("Trading requires Prisma UI.");
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
    if (!EnsureView())
    {
        if (message.Session && (message.Phase == Trade::Pending || message.Phase == Trade::Open))
        {
            RequestTrade cancel{};
            cancel.Action=Trade::Cancel; cancel.Session=message.Session; cancel.Revision=message.Revision;
            m_world.GetTransport().Send(cancel);
        }
        m_world.GetOverlayService().SendSystemMessage("Trade unavailable: Prisma UI is not installed or could not load its view.");
        return;
    }
    m_state=message; m_show=true;
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
    m_api->Show(m_view);
    m_api->InteropCall(m_view,"updateTrade",json.c_str());
    if(!m_api->HasFocus(m_view)) m_api->Focus(m_view,false,false);
}
void TradeService::Hide() noexcept
{
    m_show=false;
    if(m_api && m_view && m_api->IsValid(m_view))
    { m_api->Unfocus(m_view); m_api->Hide(m_view); }
}
void TradeService::OnDisconnect(const DisconnectedEvent&) noexcept
{
    m_world.QueueCheckpointTask([this]() { m_state=NotifyTrade{}; Hide(); });
}
