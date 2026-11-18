#include "QuestJournal.h"
#include "DurableAppend.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Campaign
{
namespace
{
bool IsValid(const QuestEvent& acEvent) noexcept
{
    return acEvent.Quest.IsValid() && acEvent.Status <= QuestEvent::Stopped && acEvent.Type <= 11;
}
}

QuestJournal::QuestJournal(std::filesystem::path aPath)
    : m_path(std::move(aPath))
{
    Load();
}

bool QuestJournal::CanRecord(const QuestEvent& acEvent) const noexcept
{
    const auto it = m_quests.find(acEvent.Quest);
    if (it == m_quests.end())
        return acEvent.Status != QuestEvent::Stopped;

    const auto& state = it->second;
    if (state.Stopped || acEvent.Status == QuestEvent::Started)
        return false;
    if (acEvent.Status == QuestEvent::Stopped)
        return acEvent.Stage >= state.HighestStage;

    // Skyrim can explicitly set an earlier stage after the current stage has advanced.
    return !state.Stages.contains(acEvent.Stage);
}

void QuestJournal::Record(const QuestEvent& acEvent, uint32_t aOrigin, bool aIncludeStopStage)
{
    auto& state = m_quests[acEvent.Quest];
    const bool cApplyStage = acEvent.Status == QuestEvent::Stopped && aIncludeStopStage &&
                             state.Stages.insert(acEvent.Stage).second;
    if (acEvent.Status == QuestEvent::Stopped)
        state.Stopped = true;
    else if (acEvent.Status == QuestEvent::StageUpdate)
        state.Stages.insert(acEvent.Stage);
    state.HighestStage = std::max(state.HighestStage, acEvent.Stage);

    m_records.push_back({acEvent, cApplyStage, aOrigin});
}

void QuestJournal::Load()
{
    std::error_code error;
    m_hasFile = std::filesystem::exists(m_path, error);
    if (error)
        m_healthy = false;
    if (!m_hasFile)
        return;

    std::ifstream file(m_path, std::ios::binary);
    std::string line;
    size_t lineNumber = 0;
    bool seenVersion2 = false;
    while (std::getline(file, line))
    {
        ++lineNumber;
        std::istringstream input(line);
        input.imbue(std::locale::classic());
        unsigned int version{}, lite{}, stage{}, status{}, type{};
        uint64_t sequence{};
        QuestEvent event;
        if (file.eof() || !(input >> version) || (version != 1 && version != 2) ||
            (version == 1 && seenVersion2) ||
            (version == 2 && (!(input >> sequence) || sequence != lineNumber)) ||
            !(input >> std::quoted(event.Quest.Plugin) >> lite >> event.Quest.BaseId >> stage >> status >> type) ||
            lite > 1 || stage > UINT16_MAX || status > QuestEvent::Stopped || type > 11)
        {
            m_errorLine = lineNumber;
            break;
        }

        event.Quest.Lite = lite != 0;
        event.Stage = static_cast<uint16_t>(stage);
        event.Status = static_cast<QuestEvent::Kind>(status);
        event.Type = static_cast<uint8_t>(type);
        input >> std::ws;
        if (!input.eof() || !IsValid(event) || !CanRecord(event))
        {
            m_errorLine = lineNumber;
            break;
        }

        seenVersion2 |= version == 2;
        Record(event, 0, version == 2);
    }

    m_healthy = m_healthy && file.eof() && !file.bad() && m_errorLine == 0;
    if (!m_healthy)
    {
        m_records.clear();
        m_quests.clear();
    }
}

QuestJournal::Result QuestJournal::Append(const QuestEvent& acEvent, uint32_t aOrigin) noexcept
{
    if (!m_healthy)
        return Result::Unavailable;
    if (!IsValid(acEvent) || !CanRecord(acEvent))
        return Result::Ignored;

    std::ostringstream record;
    record.imbue(std::locale::classic());
    record << 2 << ' ' << m_records.size() + 1 << ' ' << std::quoted(acEvent.Quest.Plugin) << ' '
           << acEvent.Quest.Lite << ' ' << acEvent.Quest.BaseId << ' ' << acEvent.Stage << ' '
           << static_cast<unsigned int>(acEvent.Status) << ' ' << static_cast<unsigned int>(acEvent.Type) << '\n';
    if (!AppendDurably(m_path, record.str(), !m_hasFile))
    {
        m_healthy = false;
        m_errorLine = m_records.size() + 1;
        return Result::Unavailable;
    }

    m_hasFile = true;
    Record(acEvent, aOrigin, true);
    return Result::Committed;
}
}
