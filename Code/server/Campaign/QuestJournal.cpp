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
    return acEvent.Quest.IsValid() && acEvent.Status <= QuestEvent::Completed && acEvent.Type <= 11;
}
}

QuestJournal::QuestJournal(std::filesystem::path aPath)
    : m_path(std::move(aPath))
{
    Load();
}

const QuestSnapshot* QuestJournal::Find(const PluginFormKey& acKey) const noexcept
{
    const auto it = m_quests.find(acKey);
    return it == m_quests.end() ? nullptr : &it->second;
}

bool QuestJournal::CanRecord(const QuestEvent& acEvent) const noexcept
{
    const auto* pState = Find(acEvent.Quest);
    if (!pState)
        return acEvent.Status == QuestEvent::Started || acEvent.Status == QuestEvent::StageUpdate;

    if (acEvent.Status == QuestEvent::Completed)
        return !pState->Completed && acEvent.Stage >= pState->HighestStage &&
               (!pState->Stopped || acEvent.Stage == pState->HighestStage);

    if (pState->Stopped || acEvent.Status == QuestEvent::Started)
        return false;
    if (acEvent.Status == QuestEvent::Stopped)
        return acEvent.Stage >= pState->HighestStage;

    // Skyrim can explicitly set an earlier stage after the current stage has advanced.
    return !pState->Stages.contains(acEvent.Stage);
}

void QuestJournal::Record(const QuestEvent& acEvent, uint32_t aOrigin, bool aIncludeOutcomeStage)
{
    auto& state = m_quests[acEvent.Quest];
    const bool cOutcome = acEvent.Status == QuestEvent::Stopped || acEvent.Status == QuestEvent::Completed;
    const bool cApplyStage = cOutcome && aIncludeOutcomeStage && !state.Stopped &&
                             state.Stages.insert(acEvent.Stage).second;
    if (acEvent.Status == QuestEvent::Stopped)
        state.Stopped = true;
    else if (acEvent.Status == QuestEvent::Completed)
        state.Completed = true;
    else if (acEvent.Status == QuestEvent::StageUpdate)
        state.Stages.insert(acEvent.Stage);
    state.HighestStage = std::max(state.HighestStage, acEvent.Stage);

    m_records.push_back({acEvent, cApplyStage, aOrigin});
    state.Sequence = m_records.size();
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
    unsigned int previousVersion = 1;
    while (std::getline(file, line))
    {
        ++lineNumber;
        std::istringstream input(line);
        input.imbue(std::locale::classic());
        unsigned int version{}, lite{}, stage{}, status{}, type{};
        uint64_t sequence{};
        QuestEvent event;
        if (file.eof() || !(input >> version) || version < previousVersion || version > 3 ||
            (version >= 2 && (!(input >> sequence) || sequence != lineNumber)) ||
            !(input >> std::quoted(event.Quest.Plugin) >> lite >> event.Quest.BaseId >> stage >> status >> type) ||
            lite > 1 || stage > UINT16_MAX || status > (version >= 3 ? QuestEvent::Completed : QuestEvent::Stopped) || type > 11)
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

        previousVersion = version;
        Record(event, 0, version >= 2);
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
    record << 3 << ' ' << m_records.size() + 1 << ' ' << std::quoted(acEvent.Quest.Plugin) << ' '
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
