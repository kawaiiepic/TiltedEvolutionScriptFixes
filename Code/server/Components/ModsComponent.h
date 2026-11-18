#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Campaign/PluginFormKey.h>
#include <Structs/GameId.h>
#include <optional>
#include <vector>

struct Player;

namespace ESLoader
{
struct PluginData;
}

struct ModsComponent
{
    struct Entry
    {
        uint32_t id;
        uint32_t refCount;
    };

    uint32_t AddStandard(const String& acpFilename) noexcept;
    uint32_t AddLite(const String& acpFilename) noexcept;

    void AddServerMod(const ESLoader::PluginData& acData);

    std::optional<Campaign::PluginFormKey> FindFormKey(const GameId& acId, const Player& acPlayer) const noexcept;
    std::optional<Campaign::PluginFormKey> FindFormKey(const GameId& acId) const noexcept;
    std::optional<GameId> FindFormId(const Campaign::PluginFormKey& acKey) const noexcept;
    const auto& GetServerMods() const noexcept { return m_serverMods; }

    bool IsInstalled(const String& acpFileName) const noexcept;

    using TModList = TiltedPhoques::Map<String, Entry>;

private:
    uint32_t m_seed = 0;
    std::vector<Campaign::PluginFormKey> m_formPlugins;
    // Mappings of ids owned by the server
    TModList m_standardMods;
    TModList m_liteMods;

    // List of mods installed on the server.
    TModList m_serverMods;
};
