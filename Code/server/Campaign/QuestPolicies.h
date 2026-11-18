#pragma once

#include "QuestJournal.h"

namespace Campaign
{
class QuestPolicies
{
public:
    explicit QuestPolicies(const std::filesystem::path& acPath);

    bool IsShared(const QuestKey& acQuest, bool aDefault) const noexcept;
    bool IsHealthy() const noexcept { return m_healthy; }
    size_t ErrorLine() const noexcept { return m_errorLine; }
    size_t Size() const noexcept { return m_overrides.size(); }

private:
    std::map<QuestKey, bool> m_overrides;
    bool m_healthy{true};
    size_t m_errorLine{};
};
}
