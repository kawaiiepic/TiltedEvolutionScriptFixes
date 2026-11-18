#include "NpcLifeJournal.h"
#include "DurableAppend.h"

#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Campaign
{
NpcLifeJournal::NpcLifeJournal(std::filesystem::path aPath)
    : m_path(std::move(aPath))
{
    std::error_code error;
    m_hasFile = std::filesystem::exists(m_path, error);
    m_healthy = !error;
    if (!m_hasFile || error)
        return;

    std::ifstream file(m_path, std::ios::binary);
    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream input(line);
        input.imbue(std::locale::classic());
        unsigned int version{}, lite{}, dead{};
        uint64_t sequence{};
        PluginFormKey key;
        if (file.eof() || !(input >> version >> sequence >> std::quoted(key.Plugin) >> lite >> key.BaseId >> dead) ||
            version != 1 || sequence != m_sequence + 1 || lite > 1 || dead > 1)
        {
            m_healthy = false;
            break;
        }

        input >> std::ws;
        key.Lite = lite != 0;
        if (!input.eof() || !key.IsValid())
        {
            m_healthy = false;
            break;
        }
        m_deaths.insert_or_assign(std::move(key), dead != 0);
        ++m_sequence;
    }

    m_healthy = m_healthy && file.eof() && !file.bad();
    if (!m_healthy)
        m_deaths.clear();
}

std::optional<bool> NpcLifeJournal::GetDeathState(const PluginFormKey& acKey) const noexcept
{
    const auto it = m_deaths.find(acKey);
    if (it == m_deaths.end())
        return std::nullopt;
    return it->second;
}

bool NpcLifeJournal::SetDeathState(const PluginFormKey& acKey, bool aDead) noexcept
{
    if (!m_healthy || !acKey.IsValid())
        return false;
    const auto previous = GetDeathState(acKey);
    if (previous && *previous == aDead)
        return true;

    std::ostringstream record;
    record.imbue(std::locale::classic());
    record << 1 << ' ' << m_sequence + 1 << ' ' << std::quoted(acKey.Plugin) << ' '
           << acKey.Lite << ' ' << acKey.BaseId << ' ' << aDead << '\n';
    if (!AppendDurably(m_path, record.str(), !m_hasFile))
    {
        m_healthy = false;
        return false;
    }

    m_hasFile = true;
    ++m_sequence;
    m_deaths.insert_or_assign(acKey, aDead);
    return true;
}
}
