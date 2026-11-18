
#include <Components.h>
#include <es_loader/ESLoader.h>

uint32_t ModsComponent::AddStandard(const String& acpFilename) noexcept
{
    const auto itor = m_standardMods.find(acpFilename);
    if (itor != std::end(m_standardMods))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto id = m_seed++;
    m_standardMods.emplace(acpFilename, Entry{id, 1});
    m_formPlugins.push_back({acpFilename.c_str(), false});

    return id;
}

uint32_t ModsComponent::AddLite(const String& acpFilename) noexcept
{
    const auto itor = m_liteMods.find(acpFilename);
    if (itor != std::end(m_liteMods))
    {
        itor.value().refCount++;
        return itor->second.id;
    }

    const auto id = m_seed++;
    m_liteMods.emplace(acpFilename, Entry{id, 1});
    m_formPlugins.push_back({acpFilename.c_str(), true});

    return id;
}

void ModsComponent::AddServerMod(const ESLoader::PluginData& acData)
{
    // kind of a hack since we want to store both, so we take the two byte value
    m_serverMods.emplace(acData.m_filename, Entry{acData.m_liteId, 1});
}

bool ModsComponent::IsInstalled(const String& acpFilename) const noexcept
{
    auto it = std::find_if(m_serverMods.begin(), m_serverMods.end(), [&](const TModList::value_type& aEntry) { return aEntry.first == acpFilename; });

    return it != m_serverMods.end();
}

std::optional<Campaign::PluginFormKey> ModsComponent::FindFormKey(const GameId& acId, const Player& acPlayer) const noexcept
{
    if (!acPlayer.HasModId(acId.ModId))
        return std::nullopt;
    return FindFormKey(acId);
}

std::optional<Campaign::PluginFormKey> ModsComponent::FindFormKey(const GameId& acId) const noexcept
{
    if (acId.ModId >= m_formPlugins.size())
        return std::nullopt;

    const auto& plugin = m_formPlugins[acId.ModId];
    auto key = plugin;
    key.BaseId = acId.BaseId;
    if (!key.IsValid())
        return std::nullopt;
    return key;
}

std::optional<GameId> ModsComponent::FindFormId(const Campaign::PluginFormKey& acKey) const noexcept
{
    const auto& list = acKey.Lite ? m_liteMods : m_standardMods;
    const auto it = list.find(String(acKey.Plugin.c_str()));
    if (it == list.end())
        return std::nullopt;
    return GameId(it->second.id, acKey.BaseId);
}
