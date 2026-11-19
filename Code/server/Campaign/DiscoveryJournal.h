#pragma once

#include "PluginFormKey.h"
#include <filesystem>
#include <set>

namespace Campaign
{
class DiscoveryJournal
{
public:
    enum class Result { Known, Added, Rejected };

    explicit DiscoveryJournal(std::filesystem::path aPath);
    Result Discover(const PluginFormKey& acKey);
    const std::set<PluginFormKey>& GetDiscoveries() const noexcept { return m_discoveries; }
    bool IsHealthy() const noexcept { return m_healthy; }

    static constexpr size_t MaxDiscoveries = 16384;

private:
    std::filesystem::path m_path;
    std::set<PluginFormKey> m_discoveries;
    bool m_hasFile{};
    bool m_healthy{true};
};
}
