#pragma once

#include "PluginFormKey.h"

#include <filesystem>
#include <map>

namespace Campaign
{
struct LockState
{
    bool Locked{};
    uint8_t Level{};

    bool operator==(const LockState&) const = default;
};

class LockJournal
{
public:
    explicit LockJournal(std::filesystem::path aPath);

    const LockState* Find(const PluginFormKey& acKey) const noexcept;
    bool Set(const PluginFormKey& acKey, LockState aState) noexcept;
    bool IsHealthy() const noexcept { return m_healthy; }
    size_t ErrorLine() const noexcept { return m_errorLine; }

private:
    std::filesystem::path m_path;
    std::map<PluginFormKey, LockState> m_locks;
    size_t m_sequence{};
    size_t m_errorLine{};
    bool m_hasFile{};
    bool m_healthy{true};
};
}
