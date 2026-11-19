#include <Services/CalendarService.h>

#include <GameServer.h>
#include <World.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ServerTimeSettings.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <charconv>
#include <cmath>

#include "Game/Player.h"

namespace
{
void SendRestMessage(const Player* apPlayer, std::string_view aText)
{
    NotifyChatMessageBroadcast notify{};
    notify.MessageType = kSystemMessage;
    notify.ChatMessage.assign(aText.begin(), aText.end());
    if (apPlayer)
        apPlayer->Send(notify);
    else
        GameServer::Get()->SendToLoaded(notify);
}
}

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::OnUpdate>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&CalendarService::OnPlayerJoin>(this);
}

void CalendarService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_lastTick)
        m_lastTick = GameServer::Get()->GetTick();

    auto now = GameServer::Get()->GetTick();

    // client got ahead, we wait
    if (now < m_lastTick)
        return;

    auto delta = now - m_lastTick;
    m_lastTick = now;
    m_dateTime.Update(delta);
    UpdateRestVote();
}

std::set<uint32_t> CalendarService::GetRestVoters() const
{
    std::set<uint32_t> voters;
    for (const auto* pPlayer : m_world.GetPlayerManager())
        if (pPlayer->GetCharacter() && pPlayer->GetCellComponent())
            voters.insert(pPlayer->GetId());
    return voters;
}

void CalendarService::UpdateRestVote() noexcept
{
    if (!m_restVote)
        return;

    if (std::chrono::steady_clock::now() < m_restVote->Deadline)
    {
        size_t activeVoters = 0;
        bool changed = false;
        for (const auto* pPlayer : m_world.GetPlayerManager())
        {
            if (!pPlayer->GetCharacter() || !pPlayer->GetCellComponent())
                continue;
            ++activeVoters;
            changed |= !m_restVote->Voters.contains(pPlayer->GetId());
        }
        if (!changed && activeVoters == m_restVote->Voters.size())
            return;
    }

    m_restVote.reset();
    SendRestMessage(nullptr, "Rest vote expired or the active players changed.");
}

bool CalendarService::HandleRestCommand(const Player& acPlayer, std::string_view aCommand) noexcept
{
    if (aCommand != "/rest" && !aCommand.starts_with("/rest "))
        return false;

    UpdateRestVote();
    aCommand = aCommand.size() > 5 ? aCommand.substr(6) : std::string_view{};
    const auto separator = aCommand.find(' ');
    const auto action = aCommand.substr(0, separator);
    const auto number = separator == aCommand.npos ? std::string_view{} : aCommand.substr(separator + 1);
    uint32_t value{};
    if (number.empty())
    {
        SendRestMessage(&acPlayer, "Use /rest sleep HOURS, /rest wait HOURS, /rest yes ID, /rest no ID, or /rest cancel ID.");
        return true;
    }
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !value)
    {
        SendRestMessage(&acPlayer, "Enter a positive whole number for hours or vote ID.");
        return true;
    }

    const uint32_t playerId = acPlayer.GetId();
    if (action == "sleep" || action == "wait")
    {
        if (value > 24 || m_restVote || !acPlayer.GetCharacter() || !acPlayer.GetCellComponent())
        {
            SendRestMessage(&acPlayer, "Request 1 to 24 hours while in the world and no other vote is active.");
            return true;
        }

        if (++m_nextRestVote == 0)
            ++m_nextRestVote;
        m_restVote = RestVote{m_nextRestVote, playerId, value, action == "sleep", GetRestVoters(), {playerId},
                              std::chrono::steady_clock::now() + std::chrono::seconds(30)};
        SendRestMessage(nullptr, fmt::format("{} proposed {} for {} hour(s). Vote within 30 seconds with /rest yes {} or /rest no {}.",
            acPlayer.GetUsername().c_str(), action, value, m_nextRestVote, m_nextRestVote));
    }
    else
    {
        if (!m_restVote || m_restVote->Id != value || !m_restVote->Voters.contains(playerId))
        {
            SendRestMessage(&acPlayer, "That rest vote is no longer open to you.");
            return true;
        }

        if (action == "no" || (action == "cancel" && playerId == m_restVote->Organizer))
        {
            m_restVote.reset();
            SendRestMessage(nullptr, "Rest vote canceled.");
            return true;
        }

        if (action != "yes")
        {
            SendRestMessage(&acPlayer, "Use /rest yes ID or /rest no ID. The organizer can use /rest cancel ID.");
            return true;
        }
        m_restVote->Approved.insert(playerId);
    }

    if (m_restVote->Approved == m_restVote->Voters)
    {
        const uint32_t hours = m_restVote->Hours;
        const bool sleep = m_restVote->Sleep;
        m_restVote.reset();
        m_dateTime.AdvanceHours(static_cast<float>(hours));
        SendTimeResync(static_cast<uint8_t>(hours));
        SendRestMessage(nullptr, fmt::format("Group {} approved. The calendar advanced by {} hour(s).", sleep ? "sleep" : "wait", hours));
    }
    return true;
}

void CalendarService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    ServerTimeSettings timeMsg;
    // Validate the model before GetTimeInDays iterates the months.
    const auto& playerTime = acEvent.PlayerTime.m_timeModel;
    const bool cValidTime = playerTime.Month < 12 && playerTime.Day >= 1 &&
        playerTime.Day <= DateTime::GetNumberOfDaysByMonthIndex(playerTime.Month) &&
        playerTime.Time >= 0.f && playerTime.Time < 24.f;
    const bool cPlayerHasFurthestTime = cValidTime && acEvent.PlayerTime.GetTimeInDays() > m_dateTime.GetTimeInDays();

    if (cPlayerHasFurthestTime)
    {
        // Note that this doesn't set timescale because the server config should set that.
        if (!m_timeSetFromFirstPlayer)
        {
            m_dateTime.m_timeModel.Time = acEvent.PlayerTime.m_timeModel.Time;
            m_timeSetFromFirstPlayer = true;
        }
        m_dateTime.m_timeModel.Day = acEvent.PlayerTime.m_timeModel.Day;
        m_dateTime.m_timeModel.Month = acEvent.PlayerTime.m_timeModel.Month;
        m_dateTime.m_timeModel.Year = acEvent.PlayerTime.m_timeModel.Year;
    }
    timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
    timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
    timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
    timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
    timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;

    if (cPlayerHasFurthestTime)
        GameServer::Get()->SendToPlayers(timeMsg);
    else
        acEvent.pPlayer->Send(timeMsg);
}

bool CalendarService::SetTime(int aHours, int aMinutes, float aScale) noexcept
{
    if (aHours < 0 || aHours > 23 || aMinutes < 0 || aMinutes > 59 || !(aScale >= 0.f && aScale <= 1000.f))
        return false;

    m_dateTime.m_timeModel.TimeScale = aScale;
    m_dateTime.m_timeModel.Time = static_cast<float>(aHours) + static_cast<float>(aMinutes) / 60.f;
    SendTimeResync();
    m_world.GetScriptService().HandleSetTime(aHours, aMinutes, aScale);
    return true;
}

bool CalendarService::SetDate(int aDay, int aMonth, float aYear) noexcept
{
    if (aMonth >= 0 && aMonth < 12 && aYear >= 0 && aYear <= 999)
    {
        const int cMaxDays = static_cast<int>(DateTime::GetNumberOfDaysByMonthIndex(aMonth));
        if (aDay >= 1 && aDay <= cMaxDays)
        {
            m_dateTime.m_timeModel.Day = aDay;
            m_dateTime.m_timeModel.Month = aMonth;
            m_dateTime.m_timeModel.Year = aYear;
            SendTimeResync();
            return true;
        }
    }
    return false;
}

void CalendarService::SendTimeResync(uint8_t aRestHours) noexcept
{
    ServerTimeSettings timeMsg;
    timeMsg.RestHours = aRestHours;
    timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
    timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
    timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
    timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
    timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;
    GameServer::Get()->SendToLoaded(timeMsg);
}

CalendarService::TTime CalendarService::GetTime() const noexcept
{
    // Round to the nearest displayed minute so SetTime round trips.
    const int cMinutes = static_cast<int>(std::lround(m_dateTime.m_timeModel.Time * 60.f)) % (24 * 60);
    return {cMinutes / 60, cMinutes % 60};
}

CalendarService::TTime CalendarService::GetRealTime() noexcept
{
    const auto t = std::time(nullptr);
    int h = (t / 3600) % 24;
    int m = (t / 60) % 60;
    return {h, m};
}

CalendarService::TDate CalendarService::GetDate() const noexcept
{
    return {m_dateTime.m_timeModel.Day, m_dateTime.m_timeModel.Month, m_dateTime.m_timeModel.Year};
}

bool CalendarService::SetTimeScale(float aScale) noexcept
{
    if (aScale >= 0.f && aScale <= 1000.f)
    {
        m_dateTime.m_timeModel.TimeScale = aScale;

        ServerTimeSettings timeMsg;
        timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
        timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
        timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
        timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
        timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;
        GameServer::Get()->SendToPlayers(timeMsg);
        return true;
    }

    return false;
}
