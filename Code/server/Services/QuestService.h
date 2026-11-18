#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

struct World;
struct Player;
struct PlayerJoinEvent;
struct RequestQuestUpdate;

// The server owns an ordered campaign journal. The game clients still run native quest fragments.
class QuestService
{
public:
    QuestService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    struct JournalEntry
    {
        std::string Plugin;
        bool Lite{};
        uint32_t BaseId{};
        uint16_t Stage{};
        uint8_t Status{};
        uint8_t Type{};
    };

    using QuestKey = std::tuple<std::string, bool, uint32_t>;
    struct QuestState
    {
        std::set<uint16_t> Stages;
        uint16_t HighestStage{};
        bool Stopped{};
    };

    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    bool CanRecord(const JournalEntry& acEntry) const noexcept;
    void Record(const JournalEntry& acEntry) noexcept;
    void LoadJournal() noexcept;
    bool Append(const JournalEntry& acEntry) noexcept;
    void SendEntry(const JournalEntry& acEntry, const Player& acPlayer) const noexcept;
    bool ResolveEntry(const RequestQuestUpdate& acRequest, const Player& acPlayer, JournalEntry& aEntry) const noexcept;

    World& m_world;
    std::vector<JournalEntry> m_journal;
    std::map<QuestKey, QuestState> m_quests;
    bool m_journalHealthy{true};

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_joinConnection;
};
