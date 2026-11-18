#include "GameServer.h"

namespace Script
{
void CreateQuestServiceBindings(sol::state_view aState)
{
    auto questType =
        aState.new_usertype<QuestService>("QuestService", sol::meta_function::construct, sol::no_constructor);

    questType["get"] = []() -> QuestService& { return GameServer::Get()->GetWorld().GetQuestService(); };
    questType["GetCampaignQuest"] = [](const QuestService& acService, const std::string& acPlugin, bool aLite,
                                       uint32_t aBaseId, sol::this_state aLua) -> sol::object
    {
        const auto* pQuest = acService.GetCampaignQuest({acPlugin, aLite, aBaseId});
        if (!pQuest)
            return sol::nil;

        sol::state_view lua(aLua);
        sol::table snapshot = lua.create_table();
        snapshot["sequence"] = pQuest->Sequence;
        snapshot["highestStage"] = pQuest->HighestStage;
        snapshot["stopped"] = pQuest->Stopped;
        sol::table stages = lua.create_table();
        size_t index = 1;
        for (uint16_t stage : pQuest->Stages)
            stages[index++] = stage;
        snapshot["stages"] = stages;
        return snapshot;
    };
}
} // namespace Script
