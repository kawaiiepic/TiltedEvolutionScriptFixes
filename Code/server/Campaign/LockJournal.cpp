#include "LockJournal.h"
#include "DurableAppend.h"

#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Campaign
{
LockJournal::LockJournal(std::filesystem::path aPath)
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
        unsigned int version{}, lite{}, locked{}, level{};
        size_t sequence{};
        PluginFormKey key;
        if (file.eof() || !(input >> version >> sequence >> std::quoted(key.Plugin) >> lite >> key.BaseId >> locked >> level) ||
            version != 1 || sequence != m_sequence + 1 || lite > 1 || locked > 1 || level > UINT8_MAX)
        {
            m_errorLine = m_sequence + 1;
            break;
        }

        key.Lite = lite != 0;
        input >> std::ws;
        if (!input.eof() || !key.IsValid())
        {
            m_errorLine = m_sequence + 1;
            break;
        }
        m_locks.insert_or_assign(std::move(key), LockState{locked != 0, static_cast<uint8_t>(level)});
        ++m_sequence;
    }

    m_healthy = file.eof() && !file.bad() && m_errorLine == 0;
    if (!m_healthy)
        m_locks.clear();
}

const LockState* LockJournal::Find(const PluginFormKey& acKey) const noexcept
{
    const auto it = m_locks.find(acKey);
    return it == m_locks.end() ? nullptr : &it->second;
}

bool LockJournal::Set(const PluginFormKey& acKey, LockState aState) noexcept
{
    if (!m_healthy || !acKey.IsValid())
        return false;
    const auto* pPrevious = Find(acKey);
    if (pPrevious && *pPrevious == aState)
        return true;

    std::ostringstream record;
    record.imbue(std::locale::classic());
    record << 1 << ' ' << m_sequence + 1 << ' ' << std::quoted(acKey.Plugin) << ' ' << acKey.Lite << ' '
           << acKey.BaseId << ' ' << aState.Locked << ' ' << static_cast<unsigned int>(aState.Level) << '\n';
    if (!AppendDurably(m_path, record.str(), !m_hasFile))
    {
        m_healthy = false;
        m_errorLine = m_sequence + 1;
        return false;
    }

    m_hasFile = true;
    ++m_sequence;
    m_locks.insert_or_assign(acKey, aState);
    return true;
}
}
