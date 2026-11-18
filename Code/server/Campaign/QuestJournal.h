#pragma once

#include "PluginFormKey.h"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Campaign
{
struct QuestEvent
{
    enum Kind : uint8_t { StageUpdate, Started, Stopped, Completed };

    PluginFormKey Quest;
    uint16_t Stage{};
    Kind Status{};
    uint8_t Type{};
};

struct JournalRecord
{
    QuestEvent Event;
    bool ApplyStageBeforeOutcome{};
    uint32_t Origin{}; // Only meaningful within the server process that accepted the event.
};

struct QuestSnapshot
{
    std::set<uint16_t> Stages;
    uint16_t HighestStage{};
    bool Stopped{};
    bool Completed{};
    uint64_t Sequence{};
};

class QuestJournal
{
public:
    enum class Result { Committed, Ignored, Unavailable };

    explicit QuestJournal(std::filesystem::path aPath);
    Result Append(const QuestEvent& acEvent, uint32_t aOrigin) noexcept;
    const std::vector<JournalRecord>& Records() const noexcept { return m_records; }
    const QuestSnapshot* Find(const PluginFormKey& acKey) const noexcept;
    bool IsHealthy() const noexcept { return m_healthy; }
    size_t ErrorLine() const noexcept { return m_errorLine; }

private:
    bool CanRecord(const QuestEvent& acEvent) const noexcept;
    void Record(const QuestEvent& acEvent, uint32_t aOrigin, bool aIncludeOutcomeStage);
    void Load();

    std::filesystem::path m_path;
    std::vector<JournalRecord> m_records;
    std::map<PluginFormKey, QuestSnapshot> m_quests;
    bool m_healthy{true};
    bool m_hasFile{};
    size_t m_errorLine{};
};
}
