#include "FormSyncPolicies.h"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Campaign
{
FormSyncPolicies::FormSyncPolicies(const std::filesystem::path& acPath)
{
    std::error_code error;
    const bool cExists = std::filesystem::exists(acPath, error);
    m_healthy = !error;
    if (!cExists || error)
        return;

    std::ifstream file(acPath);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        std::istringstream input(line);
        input.imbue(std::locale::classic());
        input >> std::ws;
        if (input.eof() || input.peek() == '#')
            continue;

        std::string mode, format, base;
        PluginFormKey key;
        if (!(input >> mode >> format >> std::quoted(key.Plugin) >> base) ||
            (mode != "shared" && mode != "personal") ||
            (format != "standard" && format != "light"))
        {
            m_errorLine = lineNumber;
            break;
        }

        input >> std::ws;
        key.Lite = format == "light";
        const char* pStart = base.data();
        const char* pEnd = pStart + base.size();
        if (base.starts_with("0x") || base.starts_with("0X"))
            pStart += 2;
        const auto result = std::from_chars(pStart, pEnd, key.BaseId, 16);
        if ((!input.eof() && input.peek() != '#') || result.ec != std::errc{} || result.ptr != pEnd ||
            !key.IsValid() ||
            !m_overrides.emplace(std::move(key), mode == "shared").second)
        {
            m_errorLine = lineNumber;
            break;
        }
    }

    m_healthy = file.eof() && !file.bad() && m_errorLine == 0;
    if (!m_healthy)
        m_overrides.clear();
}

bool FormSyncPolicies::IsShared(const PluginFormKey& acQuest, bool aDefault) const noexcept
{
    if (!m_healthy)
        return false;
    const auto it = m_overrides.find(acQuest);
    return it == m_overrides.end() ? aDefault : it->second;
}
}
