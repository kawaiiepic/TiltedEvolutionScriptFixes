#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/LockData.h>
#include <Game/Player.h>
#include <Campaign/PluginFormKey.h>
#include <optional>

struct ObjectComponent
{
    std::optional<Campaign::PluginFormKey> CampaignLock;
    std::optional<Campaign::PluginFormKey> CampaignStock;
    LockData CurrentLockData{};
};
