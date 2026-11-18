#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Campaign
{
struct QuestKey
{
    std::string Plugin;
    bool Lite{};
    uint32_t BaseId{};

    auto operator<=>(const QuestKey&) const = default;
};

struct QuestEvent
{
    enum Kind : uint8_t { StageUpdate, Started, Stopped };

    QuestKey Quest;
    uint16_t Stage{};
    Kind Status{};
    uint8_t Type{};
};

struct JournalRecord
{
    QuestEvent Event;
    bool ApplyStageBeforeStop{};
    uint32_t Origin{}; // Only meaningful within the server process that accepted the event.
};

class QuestJournal
{
public:
    enum class Result { Committed, Ignored, Unavailable };

    explicit QuestJournal(std::filesystem::path aPath);
    Result Append(const QuestEvent& acEvent, uint32_t aOrigin) noexcept;
    const std::vector<JournalRecord>& Records() const noexcept { return m_records; }
    bool IsHealthy() const noexcept { return m_healthy; }
    size_t ErrorLine() const noexcept { return m_errorLine; }

private:
    struct QuestState
    {
        std::set<uint16_t> Stages;
        uint16_t HighestStage{};
        bool Stopped{};
    };

    bool CanRecord(const QuestEvent& acEvent) const noexcept;
    void Record(const QuestEvent& acEvent, uint32_t aOrigin, bool aIncludeStopStage);
    void Load();

    std::filesystem::path m_path;
    std::vector<JournalRecord> m_records;
    std::map<QuestKey, QuestState> m_quests;
    bool m_healthy{true};
    bool m_hasFile{};
    size_t m_errorLine{};
};
}
