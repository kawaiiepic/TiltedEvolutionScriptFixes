#pragma once

#include <filesystem>
#include <string>

namespace Campaign
{
bool AppendDurably(const std::filesystem::path& acPath, const std::string& acRecord, bool aNewFile) noexcept;
}
