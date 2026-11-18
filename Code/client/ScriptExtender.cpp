
#include <ScriptExtender.h>
#include <TiltedOnlinePCH.h>
#include <VersionDb.h>

namespace
{
constexpr char kScriptExtenderEntrypoint[] = "StartSKSE";

// AE+ only
// Use this to raise the SKSE baseline
constexpr int kSKSEMinBuild = 20100;

HMODULE g_SKSEModuleHandle{nullptr};

struct FileVersion
{
    static constexpr uint8_t scVersionSize = 4;
    DWORD versions[scVersionSize];
};

int GetFileVersion(const std::filesystem::path& acFilePath, FileVersion& aVersion)
{
    const auto filename = acFilePath.c_str();

    DWORD dwHandle = 0, sz = GetFileVersionInfoSizeW(filename, &dwHandle);
    if (0 == sz)
    {
        return 1;
    }
    std::string buf(sz, '\0');
    if (!GetFileVersionInfoW(filename, dwHandle, sz, &buf[0]))
    {
        return 2;
    }
    VS_FIXEDFILEINFO* pvi;
    sz = sizeof(VS_FIXEDFILEINFO);
    if (!VerQueryValueA(&buf[0], "\\", reinterpret_cast<LPVOID*>(&pvi), reinterpret_cast<unsigned int*>(&sz)))
    {
        return 3;
    }

    aVersion.versions[0] = pvi->dwProductVersionMS >> 16;
    aVersion.versions[1] = pvi->dwFileVersionMS & 0xFFFF;
    aVersion.versions[2] = pvi->dwFileVersionLS >> 16;
    aVersion.versions[3] = pvi->dwFileVersionLS & 0xFFFF;

    return 0;
}

std::string GetSKSEStyleExeVersion()
{
    auto version = VersionDb::Get().GetLoadedVersionString();
    // SKSE names its runtime DLL with the major, minor and revision only.
    const auto patch = version.rfind('.');
    if (patch == std::string::npos)
        return {};
    version.resize(patch);
    std::replace(version.begin(), version.end(), '.', '_');
    return version;
}
} // namespace

bool IsScriptExtenderLoaded()
{
    return g_SKSEModuleHandle;
}

void LoadScriptExtender()
{
    const auto version = GetSKSEStyleExeVersion();
    if (version.empty())
        return;

    std::error_code error;
    const auto gameDir = std::filesystem::current_path(error);
    if (error)
        return;

    const auto name = "skse64_" + version;
    auto path = gameDir / (name + ".dll");
    if (!std::filesystem::is_regular_file(path, error))
    {
        path = gameDir / (name + "_winstore.dll");
        if (!std::filesystem::is_regular_file(path, error))
            return;
    }

    FileVersion fileVersion;
    if (GetFileVersion(path, fileVersion) != 0)
    {
        spdlog::error("Unable to verify Script Extender version");
        return;
    }

    auto skseVersion = fmt::format("v{}.{}.{}.{}", fileVersion.versions[0], fileVersion.versions[1], fileVersion.versions[2], fileVersion.versions[3]);

    // nice try.
    int SkseVCum = fileVersion.versions[0] * 1000000 + fileVersion.versions[1] * 10000 + fileVersion.versions[2] * 100 + fileVersion.versions[3];
    if (SkseVCum < kSKSEMinBuild)
    {
        spdlog::error("Pre anniversary Script Extender is unsupported");
        return;
    }

    if (HMODULE module = LoadLibraryW(path.c_str()))
    {
        if (auto* pStartSKSE = reinterpret_cast<void (*)()>(GetProcAddress(module, kScriptExtenderEntrypoint)))
        {
            spdlog::info(
                "Installing SKSE {} startup hooks... be aware that messages that start without a colored "
                "[timestamp] prefix are logs from the Script Extender and its loaded mods.",
                skseVersion);
            g_SKSEModuleHandle = module;
            pStartSKSE();
            spdlog::info("SKSE startup hooks installed; initialization will continue during game startup");
        }
        else
        {
            spdlog::warn("SKSE dll doesn't expose StartSKSE(), it may be outdated.");
            FreeLibrary(module);
        }
    }
    else
    {
        spdlog::error("Failed to load {}! Check your privileges or re-download the Script Extender files.", path.string());
    }
}
