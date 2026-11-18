#include <GameServer.h>
#include <Components.h>
#include <World.h>
#include <Services/QuestService.h>
#include <Events/PlayerJoinEvent.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Setting.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
Console::Setting bEnableMiscQuestSync{"Gameplay:bEnableMiscQuestSync", "(Experimental) Share miscellaneous and hidden quests", false};

constexpr const char* kJournalPath = "shared-quests.log";

bool IsSharedType(uint8_t aType) noexcept
{
    return aType <= 11 && ((aType != 0 && aType != 6) || bEnableMiscQuestSync);
}
}

QuestService::QuestService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    LoadJournal();
    m_questUpdateConnection = aDispatcher.sink<PacketEvent<RequestQuestUpdate>>().connect<&QuestService::OnQuestChanges>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&QuestService::OnPlayerJoin>(this);
}

bool QuestService::ResolveEntry(const RequestQuestUpdate& acRequest, const Player& acPlayer, JournalEntry& aEntry) const noexcept
{
    if (!acPlayer.HasModId(acRequest.Id.ModId) || acRequest.Id.BaseId == 0)
        return false;

    const auto& mods = m_world.ctx().at<ModsComponent>();
    for (bool lite : {false, true})
    {
        const auto& list = lite ? mods.GetLiteMods() : mods.GetStandardMods();
        for (const auto& [name, mod] : list)
        {
            if (mod.id != acRequest.Id.ModId)
                continue;
            if (acRequest.Id.BaseId > (lite ? 0xFFFu : 0xFFFFFFu))
                return false;

            aEntry = {name.c_str(), lite, acRequest.Id.BaseId, acRequest.Stage, acRequest.Status, acRequest.ClientQuestType};
            return true;
        }
    }
    return false;
}

bool QuestService::CanRecord(const JournalEntry& acEntry) const noexcept
{
    if (acEntry.Status > RequestQuestUpdate::Stopped || acEntry.Type > 11)
        return false;

    const auto it = m_quests.find({acEntry.Plugin, acEntry.Lite, acEntry.BaseId});
    if (it == m_quests.end())
        return acEntry.Status != RequestQuestUpdate::Stopped;

    const auto& state = it->second;
    // Do not reopen a stopped quest on a duplicate start from a late client. Repeatable
    // quest instances need explicit generations before they can safely share a campaign.
    if (state.Stopped || acEntry.Status == RequestQuestUpdate::Started)
        return false;
    if (acEntry.Status == RequestQuestUpdate::Stopped)
        return acEntry.Stage >= state.HighestStage;

    return state.Stages.count(acEntry.Stage) == 0;
}

void QuestService::Record(const JournalEntry& acEntry) noexcept
{
    auto& state = m_quests[{acEntry.Plugin, acEntry.Lite, acEntry.BaseId}];
    if (acEntry.Status == RequestQuestUpdate::Stopped)
        state.Stopped = true;
    else
    {
        // Keep explicit stage events even when a start reported the same current stage.
        if (acEntry.Status == RequestQuestUpdate::StageUpdate)
            state.Stages.insert(acEntry.Stage);
        state.HighestStage = std::max(state.HighestStage, acEntry.Stage);
    }

    m_journal.push_back(acEntry);
}

void QuestService::LoadJournal() noexcept
{
    std::error_code error;
    const bool cExists = std::filesystem::exists(kJournalPath, error);
    if (!error && !cExists)
        return;

    std::ifstream file(kJournalPath);
    std::string line;
    size_t lineNumber = 0;

    while (std::getline(file, line))
    {
        ++lineNumber;
        std::istringstream input(line);
        JournalEntry entry;
        unsigned int version{}, lite{}, stage{}, status{}, type{};
        if (!(input >> version >> std::quoted(entry.Plugin) >> lite >> entry.BaseId >> stage >> status >> type) ||
            version != 1 || entry.Plugin.empty() || lite > 1 || entry.BaseId == 0 ||
            entry.BaseId > (lite ? 0xFFFu : 0xFFFFFFu) || stage > 0xFFFFu ||
            status > RequestQuestUpdate::Stopped || type > 11)
        {
            m_journalHealthy = false;
            break;
        }
        input >> std::ws;
        if (!input.eof())
        {
            m_journalHealthy = false;
            break;
        }

        entry.Lite = lite != 0;
        entry.Stage = static_cast<uint16_t>(stage);
        entry.Status = static_cast<uint8_t>(status);
        entry.Type = static_cast<uint8_t>(type);
        if (!CanRecord(entry))
        {
            m_journalHealthy = false;
            break;
        }

        Record(entry);
    }

    if (!file.eof() || error)
        m_journalHealthy = false;

    if (!m_journalHealthy)
    {
        m_journal.clear();
        m_quests.clear();
        spdlog::error("Shared quest journal is unreadable or invalid near line {}. Quest sync disabled; repair {} before restarting.", lineNumber, kJournalPath);
        return;
    }
    spdlog::info("Loaded {} shared quest events", m_journal.size());
}

bool QuestService::Append(const JournalEntry& acEntry) noexcept
{
    if (!m_journalHealthy)
        return false;

    std::ofstream file(kJournalPath, std::ios::app);
    file << 1 << ' ' << std::quoted(acEntry.Plugin) << ' ' << acEntry.Lite << ' '
         << acEntry.BaseId << ' ' << acEntry.Stage << ' ' << static_cast<unsigned int>(acEntry.Status)
         << ' ' << static_cast<unsigned int>(acEntry.Type) << '\n';
    file.close();
    if (!file)
    {
        m_journalHealthy = false;
        spdlog::error("Unable to write {}. Shared quest sync disabled until the journal is repaired.", kJournalPath);
        return false;
    }

    Record(acEntry);
    return true;
}

void QuestService::SendEntry(const JournalEntry& acEntry, const Player& acPlayer) const noexcept
{
    if (!IsSharedType(acEntry.Type))
        return;

    // Network mod IDs are assigned on connection and can change after a restart.
    const auto& mods = m_world.ctx().at<ModsComponent>();
    const auto& list = acEntry.Lite ? mods.GetLiteMods() : mods.GetStandardMods();
    const auto it = list.find(String(acEntry.Plugin.c_str()));
    if (it == list.end() || !acPlayer.HasModId(it->second.id))
        return;

    NotifyQuestUpdate notify{};
    notify.Id = GameId(it->second.id, acEntry.BaseId);
    notify.Stage = acEntry.Stage;
    notify.Status = acEntry.Status;
    notify.ClientQuestType = acEntry.Type;
    acPlayer.Send(notify);
}

void QuestService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    if (!m_journalHealthy)
        return;

    // Authentication has already supplied the client with its mod mapping.
    for (const auto& entry : m_journal)
        SendEntry(entry, *acEvent.pPlayer);
}

void QuestService::OnQuestChanges(const PacketEvent<RequestQuestUpdate>& acMessage) noexcept
{
    const auto& message = acMessage.Packet;
    if (!m_journalHealthy || !IsSharedType(message.ClientQuestType) || message.Status > RequestQuestUpdate::Stopped)
        return;

    JournalEntry entry;
    if (!ResolveEntry(message, *acMessage.pPlayer, entry))
        return;

    // Preserve the existing per-player active quest log for other services, independently
    // of the campaign's historical journal and its duplicate suppression.
    auto& entries = acMessage.pPlayer->GetQuestLogComponent().QuestContent.Entries;
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.Id == message.Id; });
    if (message.Status == RequestQuestUpdate::Stopped)
    {
        if (it != entries.end())
            entries.erase(it);
    }
    else if (it == entries.end())
    {
        auto& record = entries.emplace_back();
        record.Id = message.Id;
        record.Stage = message.Stage;
    }
    else
        it->Stage = std::max(it->Stage, message.Stage);

    if (!CanRecord(entry))
        return;

    // A stop reports the current stage. The stage event may not have been delivered
    // separately (e.g. native scripts stop the quest immediately).
    if (entry.Status == RequestQuestUpdate::Stopped)
    {
        auto stage = entry;
        stage.Status = RequestQuestUpdate::StageUpdate;
        if (CanRecord(stage))
        {
            if (!Append(stage))
                return;
            for (const Player* pPlayer : m_world.GetPlayerManager())
            {
                if (pPlayer != acMessage.pPlayer)
                    SendEntry(stage, *pPlayer);
            }
        }
    }

    if (!Append(entry))
        return;

    spdlog::debug("Shared quest {}:{:X} stage {} status {}", entry.Plugin, entry.BaseId, entry.Stage, entry.Status);
    for (const Player* pPlayer : m_world.GetPlayerManager())
    {
        if (pPlayer != acMessage.pPlayer)
            SendEntry(entry, *pPlayer);
    }
}
