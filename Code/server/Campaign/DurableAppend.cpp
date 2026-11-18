#include "DurableAppend.h"

#include <cstdio>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Campaign
{
bool AppendDurably(const std::filesystem::path& acPath, const std::string& acRecord, bool aNewFile) noexcept
{
#ifdef _WIN32
    FILE* pFile = nullptr;
    if (_wfopen_s(&pFile, acPath.c_str(), L"ab") != 0)
        return false;
#else
    FILE* pFile = std::fopen(acPath.c_str(), "ab");
    if (!pFile)
        return false;
#endif

    bool saved = std::fwrite(acRecord.data(), 1, acRecord.size(), pFile) == acRecord.size() && std::fflush(pFile) == 0;
#ifdef _WIN32
    saved = saved && _commit(_fileno(pFile)) == 0;
#else
    saved = saved && fsync(fileno(pFile)) == 0;
#endif
    saved = std::fclose(pFile) == 0 && saved;

#ifndef _WIN32
    if (saved && aNewFile)
    {
        // The first entry also needs the new directory entry to survive a restart.
        const auto parent = acPath.parent_path();
        const int directory = open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory < 0)
            return false;
        saved = fsync(directory) == 0;
        close(directory);
    }
#else
    (void)aNewFile;
#endif
    return saved;
}
}
