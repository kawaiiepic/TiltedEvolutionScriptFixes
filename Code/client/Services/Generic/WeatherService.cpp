#include <Services/WeatherService.h>
#include <Services/TransportService.h>
#include <World.h>

#include <Events/CellChangeEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/RequestWeatherChange.h>
#include <Messages/NotifyWeatherChange.h>
#include <Sky/Sky.h>
#include <Forms/TESWeather.h>

WeatherService::WeatherService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&WeatherService::OnUpdate>(this);
    m_cellConnection = aDispatcher.sink<CellChangeEvent>().connect<&WeatherService::OnCellChange>(this);
    m_disconnectConnection = aDispatcher.sink<DisconnectedEvent>().connect<&WeatherService::OnDisconnected>(this);
    m_weatherChangeConnection = aDispatcher.sink<NotifyWeatherChange>().connect<&WeatherService::OnWeatherChange>(this);
}

void WeatherService::ReleaseOverride() noexcept
{
    if (m_appliedOverride)
    {
        if (auto* pSky = Sky::Get())
            pSky->ReleaseWeatherOverride();
        m_appliedOverride = false;
    }
}

void WeatherService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    const GameId cell = acEvent.WorldSpaceId ? acEvent.CellId : GameId{};
    if (cell == m_cell)
        return;
    ReleaseOverride();
    m_cell = cell;
    m_owner = 0;
    m_target = {};
    m_lastReported = 0;
}

void WeatherService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    ReleaseOverride();
    m_cell = {};
    m_target = {};
    m_owner = 0;
    m_lastReported = 0;
}

void WeatherService::OnWeatherChange(const NotifyWeatherChange& acMessage) noexcept
{
    if (!m_transport.IsOnline() || !m_cell || m_cell != acMessage.Cell)
        return;
    if (m_owner != acMessage.Owner)
        m_lastReported = 0;
    m_owner = acMessage.Owner;
    m_target = acMessage.Id;
    if (m_owner == m_transport.GetLocalPlayerId() || !m_target)
        ReleaseOverride();
}

void WeatherService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsOnline() || !m_cell || !m_owner)
        return;

    auto* pSky = Sky::Get();
    if (!pSky)
        return;
    const auto* pWeather = pSky->GetWeather();
    // The map supplies its own temporary weather.
    if (pWeather && pWeather->formID == 0xA6858)
        return;

    auto& mods = m_world.GetModSystem();
    if (m_owner == m_transport.GetLocalPlayerId())
    {
        if (!pWeather || pWeather->formID == m_lastReported)
            return;

        RequestWeatherChange request{};
        request.Cell = m_cell;
        if (mods.GetServerModId(pWeather->formID, request.Id) &&
            mods.GetGameId(request.Id) == pWeather->formID && m_transport.Send(request))
            m_lastReported = pWeather->formID;
        return;
    }

    if (!m_target)
        return;
    const uint32_t cFormId = mods.GetGameId(m_target);
    if (cFormId && (!pWeather || pWeather->formID != cFormId))
    {
        if (auto* pTarget = Cast<TESWeather>(TESForm::GetById(cFormId)))
        {
            pSky->ForceWeather(pTarget);
            m_appliedOverride = true;
        }
    }
}
