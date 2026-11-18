#include "ContainerJournal.h"
#include "DurableAppend.h"

#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Campaign
{
namespace
{
constexpr size_t kMaxStacks = 4096;

bool ReadKey(std::istream& aInput, PluginFormKey& aKey)
{
    unsigned int lite{};
    if (!(aInput >> std::quoted(aKey.Plugin) >> lite >> aKey.BaseId) || lite > 1)
        return false;
    aKey.Lite = lite != 0;
    return aKey.IsValid();
}

void WriteKey(std::ostream& aOutput, const PluginFormKey& acKey)
{
    aOutput << std::quoted(acKey.Plugin) << ' ' << acKey.Lite << ' ' << acKey.BaseId;
}
}

ContainerJournal::ContainerJournal(std::filesystem::path aPath)
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
        unsigned int version{};
        uint64_t sequence{};
        size_t stacks{};
        PluginFormKey container;
        if (file.eof() || !(input >> version >> sequence) || version != 1 || sequence != m_sequence + 1 ||
            !ReadKey(input, container) || !(input >> stacks) || stacks > kMaxStacks)
        {
            m_healthy = false;
            break;
        }

        ContainerStock stock;
        for (size_t i = 0; i < stacks; ++i)
        {
            PluginFormKey item;
            int32_t count{};
            if (!ReadKey(input, item) || !(input >> count) || count <= 0 || !stock.emplace(std::move(item), count).second)
            {
                m_healthy = false;
                break;
            }
        }
        input >> std::ws;
        if (!m_healthy || !input.eof())
        {
            m_healthy = false;
            break;
        }

        m_containers.insert_or_assign(std::move(container), std::move(stock));
        ++m_sequence;
    }

    m_healthy = m_healthy && file.eof() && !file.bad();
    if (!m_healthy)
        m_containers.clear();
}

const ContainerStock* ContainerJournal::Find(const PluginFormKey& acContainer) const noexcept
{
    const auto it = m_containers.find(acContainer);
    return it == m_containers.end() ? nullptr : &it->second;
}

bool ContainerJournal::Set(const PluginFormKey& acContainer, ContainerStock aStock) noexcept
{
    if (!m_healthy || !acContainer.IsValid() || aStock.size() > kMaxStacks)
        return false;
    const auto* pPrevious = Find(acContainer);
    if (pPrevious && *pPrevious == aStock)
        return true;

    std::ostringstream record;
    record.imbue(std::locale::classic());
    record << 1 << ' ' << m_sequence + 1 << ' ';
    WriteKey(record, acContainer);
    record << ' ' << aStock.size();
    for (const auto& [item, count] : aStock)
    {
        if (!item.IsValid() || count <= 0)
            return false;
        record << ' ';
        WriteKey(record, item);
        record << ' ' << count;
    }
    record << '\n';
    if (!AppendDurably(m_path, record.str(), !m_hasFile))
    {
        m_healthy = false;
        return false;
    }

    m_hasFile = true;
    ++m_sequence;
    m_containers.insert_or_assign(acContainer, std::move(aStock));
    return true;
}
}
