#pragma once

#include "PluginFormKey.h"

#include <filesystem>
#include <map>

namespace Campaign
{
class FormSyncPolicies
{
public:
    explicit FormSyncPolicies(const std::filesystem::path& acPath);

    bool IsShared(const PluginFormKey& acQuest, bool aDefault) const noexcept;
    bool IsHealthy() const noexcept { return m_healthy; }
    size_t ErrorLine() const noexcept { return m_errorLine; }
    size_t Size() const noexcept { return m_overrides.size(); }

private:
    std::map<PluginFormKey, bool> m_overrides;
    bool m_healthy{true};
    size_t m_errorLine{};
};
}
