#pragma once

#include "PluginFormKey.h"

#include <filesystem>
#include <map>

namespace Campaign
{
using ContainerStock = std::map<PluginFormKey, int32_t>;

class ContainerJournal
{
public:
    explicit ContainerJournal(std::filesystem::path aPath);

    const ContainerStock* Find(const PluginFormKey& acContainer) const noexcept;
    bool Set(const PluginFormKey& acContainer, ContainerStock aStock) noexcept;
    bool IsHealthy() const noexcept { return m_healthy; }

private:
    std::filesystem::path m_path;
    std::map<PluginFormKey, ContainerStock> m_containers;
    uint64_t m_sequence{};
    bool m_hasFile{};
    bool m_healthy{true};
};
}
