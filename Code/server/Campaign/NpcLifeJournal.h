#pragma once

#include "PluginFormKey.h"

#include <filesystem>
#include <map>
#include <optional>

namespace Campaign
{
class NpcLifeJournal
{
public:
    explicit NpcLifeJournal(std::filesystem::path aPath);
    std::optional<bool> GetDeathState(const PluginFormKey& acKey) const noexcept;
    bool SetDeathState(const PluginFormKey& acKey, bool aDead) noexcept;
    bool IsHealthy() const noexcept { return m_healthy; }

private:
    std::filesystem::path m_path;
    std::map<PluginFormKey, bool> m_deaths;
    uint64_t m_sequence{};
    bool m_hasFile{};
    bool m_healthy{true};
};
}
