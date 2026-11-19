#include "DiscoveryJournal.h"
#include "DurableAppend.h"

#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Campaign
{
DiscoveryJournal::DiscoveryJournal(std::filesystem::path aPath) : m_path(std::move(aPath))
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
        unsigned int version{}, lite{};
        size_t sequence{};
        PluginFormKey key;
        if (file.eof() || !(input >> version >> sequence >> std::quoted(key.Plugin) >> lite >> key.BaseId) ||
            version != 1 || sequence != m_discoveries.size() + 1 || lite > 1)
        {
            m_healthy = false;
            break;
        }

        key.Lite = lite != 0;
        input >> std::ws;
        if (!input.eof() || !key.IsValid() || m_discoveries.size() >= MaxDiscoveries ||
            !m_discoveries.insert(std::move(key)).second)
        {
            m_healthy = false;
            break;
        }
    }

    m_healthy = m_healthy && file.eof() && !file.bad();
    if (!m_healthy)
        m_discoveries.clear();
}

DiscoveryJournal::Result DiscoveryJournal::Discover(const PluginFormKey& acKey)
{
    if (!m_healthy || !acKey.IsValid())
        return Result::Rejected;
    if (m_discoveries.contains(acKey))
        return Result::Known;
    if (m_discoveries.size() >= MaxDiscoveries)
        return Result::Rejected;

    std::ostringstream record;
    record.imbue(std::locale::classic());
    record << 1 << ' ' << m_discoveries.size() + 1 << ' ' << std::quoted(acKey.Plugin) << ' '
           << acKey.Lite << ' ' << acKey.BaseId << '\n';
    if (!AppendDurably(m_path, record.str(), !m_hasFile))
    {
        m_healthy = false;
        return Result::Rejected;
    }

    m_hasFile = true;
    m_discoveries.insert(acKey);
    return Result::Added;
}
}
