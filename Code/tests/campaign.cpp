#include "../server/Campaign/QuestJournal.h"

#include <catch2/catch.hpp>
#include <chrono>
#include <fstream>
#include <random>

using Campaign::QuestEvent;
using Campaign::QuestJournal;

namespace
{
struct TestJournalFile
{
    TestJournalFile()
        : Directory(std::filesystem::temp_directory_path() /
                    ("st-quest-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(std::random_device{}())))
        , Path(Directory / "shared-quests.log")
    {
        REQUIRE(std::filesystem::create_directory(Directory));
    }

    ~TestJournalFile()
    {
        std::error_code error;
        std::filesystem::remove_all(Directory, error);
    }

    std::filesystem::path Directory;
    std::filesystem::path Path;
};
}

TEST_CASE("The campaign commits a final stage and stop together", "[campaign]")
{
    TestJournalFile file;
    QuestEvent event{{"Example Mod.esl", true, 0x8AB}, 10, QuestEvent::Started, 1};
    {
        QuestJournal journal(file.Path);
        REQUIRE(journal.IsHealthy());
        REQUIRE(journal.Append(event, 7) == QuestJournal::Result::Committed);
        REQUIRE(journal.Append(event, 8) == QuestJournal::Result::Ignored);
        event.Status = QuestEvent::StageUpdate;
        event.Stage = 20;
        REQUIRE(journal.Append(event, 8) == QuestJournal::Result::Committed);
        REQUIRE(journal.Append(event, 7) == QuestJournal::Result::Ignored);
        event.Status = QuestEvent::Stopped;
        event.Stage = 10;
        REQUIRE(journal.Append(event, 7) == QuestJournal::Result::Ignored);
        event.Stage = 25;
        REQUIRE(journal.Append(event, 7) == QuestJournal::Result::Committed);
        REQUIRE(journal.Records().size() == 3);
        REQUIRE(journal.Records().back().ApplyStageBeforeStop);
        REQUIRE(journal.Records().back().Origin == 7);
    }

    QuestJournal restarted(file.Path);
    REQUIRE(restarted.IsHealthy());
    REQUIRE(restarted.Records().size() == 3);
    REQUIRE(restarted.Records().back().Event.Quest == event.Quest);
    REQUIRE(restarted.Records().back().Event.Stage == 25);
    REQUIRE(restarted.Records().back().ApplyStageBeforeStop);
    REQUIRE(restarted.Records().back().Origin == 0);
    event.Status = QuestEvent::Started;
    REQUIRE(restarted.Append(event, 2) == QuestJournal::Result::Ignored);
}

TEST_CASE("The campaign reads legacy journals and rejects damaged sequences", "[campaign]")
{
    TestJournalFile file;
    {
        std::ofstream legacy(file.Path, std::ios::binary);
        legacy << "1 \"Skyrim.esm\" 0 266 10 1 1\r\n"
               << "1 \"Skyrim.esm\" 0 266 20 0 1\r\n"
               << "1 \"Skyrim.esm\" 0 266 20 2 1\r\n";
    }

    {
        QuestJournal journal(file.Path);
        REQUIRE(journal.IsHealthy());
        REQUIRE(journal.Records().size() == 3);
        REQUIRE_FALSE(journal.Records().back().ApplyStageBeforeStop);
        const QuestEvent event{{"Another.esm", false, 266}, 18, QuestEvent::StageUpdate, 11};
        REQUIRE(journal.Append(event, 1) == QuestJournal::Result::Committed);
    }
    REQUIRE(QuestJournal(file.Path).Records().size() == 4);

    SECTION("A gap is invalid")
    {
        std::ofstream damaged(file.Path, std::ios::app);
        damaged << "2 6 \"Another.esm\" 0 266 19 0 11\n";
    }
    SECTION("A torn last record is invalid even when its fields parse")
    {
        std::ofstream damaged(file.Path, std::ios::app);
        damaged << "2 5 \"Another.esm\" 0 266 19 0 11";
    }

    QuestJournal damaged(file.Path);
    REQUIRE_FALSE(damaged.IsHealthy());
    REQUIRE(damaged.ErrorLine() == 5);
    REQUIRE(damaged.Records().empty());
}

TEST_CASE("A failed campaign write never publishes an in-memory event", "[campaign]")
{
    TestJournalFile file;
    QuestJournal journal(file.Path / "missing-parent" / "shared-quests.log");
    REQUIRE(journal.IsHealthy());
    const QuestEvent event{{"Skyrim.esm", false, 266}, 20, QuestEvent::StageUpdate, 1};
    REQUIRE(journal.Append(event, 1) == QuestJournal::Result::Unavailable);
    REQUIRE_FALSE(journal.IsHealthy());
    REQUIRE(journal.ErrorLine() == 1);
    REQUIRE(journal.Records().empty());
}
