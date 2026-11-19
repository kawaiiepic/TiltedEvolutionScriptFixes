#include <TiltedOnlinePCH.h>
#include "CharacterCheckpoints.h"
#include "NativeCheckpointDriver.h"
#include "MainMenuBridge.h"
#include "../../checkpoint_bridge/CheckpointBridgeAPI.h"

#include <Messages/CharacterSave.h>
#include <Services/TransportService.h>
#include <Services/OverlayService.h>
#include <Services/InputService.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <PlayerCharacter.h>
#include <ScriptExtender.h>
#include <Misc/GameVM.h>
#include <World.h>
#include <OverlayApp.hpp>
#include <cryptopp/sha.h>
#include <cryptopp/osrng.h>
#include <zlib.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <TiltedCore/Filesystem.hpp>
#include <string_view>
#include <vector>

namespace CharacterCheckpoints
{
namespace
{
using P = CharacterSavePayload;
using Clock = std::chrono::steady_clock;
enum class Phase { Disabled, LoadingLobby, WaitingForNewGame, Choose, Selecting, Download, NewCharacter, Staged, LoadingNative, Ready, Upload, Failed };
enum class NativeOperation { None, Load, Save, LobbyLoad };
struct Stamp
{
    uint64_t Size{}, Written{};
    bool operator==(const Stamp&) const = default;
};
struct PairStamp
{
    Stamp Ess, Co;
    bool operator==(const PairStamp&) const = default;
};
struct Flight { uint8_t Action{}; uint64_t End{}; };
struct State
{
    Phase Step{Phase::Disabled};
    bool Required{}, Pending{}, NeedsBegin{}, HasRoster{};
    bool NativeEnabled{}, CaptureReady{}, AutoEnter{};
    bool AwaitCharacterCreation{}, SawRaceMenu{}, RequestedNewGame{};
    Clock::time_point NewGameDeadline{};
    uint64_t InitialNewGame{}, InitialLoadGeneration{};
    bool LobbySaveLoaded{}, PreparingFirstSave{};
    Clock::time_point CharacterReadySince{};
    NativeOperation Operation{NativeOperation::None};
    uint64_t NativeToken{};
    Clock::time_point NativeDeadline{}, NextAutoSave{}, EnterDeadline{};
    uint8_t Expected{};
    uint64_t Revision{}, Ess{}, Co{}, Offset{}, SentOffset{};
    std::vector<Flight> Flights;
    Clock::time_point TransferStart{}, LastProgress{}, CreditTime{};
    double UploadCredit{};
    uint32_t UploadRateKiB{P::MaxRateKiB};
    size_t CachedSize{}, ReusedBytes{};
    std::filesystem::path CacheFile;
    std::vector<std::array<uint8_t, 32>> BaseBlocks;
    std::array<uint8_t, 32> Digest{};
    std::vector<uint8_t> Bytes;
    std::filesystem::path Directory;
    std::string Name, Detail, CharacterLabel, Roster;
    Clock::time_point NextNameAttempt{};
    PairStamp Baseline{}, Candidate{}, Captured{};
    Clock::time_point StableSince{}, NextPoll{}, Deadline{};
};
State s;
constexpr uint32_t kAbandonedPrison = 0x21594;
NativeDriver s_driver{};
uint64_t s_nextNativeToken{};
constexpr auto kAutosaveInterval = std::chrono::minutes(5);
constexpr auto kNativeTimeout = std::chrono::minutes(2);
constexpr char kHex[] = "0123456789abcdef";

const char* PhaseName() noexcept
{
    switch (s.Step)
    {
    case Phase::Disabled: return "disabled";
    case Phase::LoadingLobby: return "starting-staging";
    case Phase::WaitingForNewGame: return "waiting-for-new-game";
    case Phase::Choose: return s.HasRoster ? "choose" : "request-roster";
    case Phase::Selecting: return "selecting";
    case Phase::Download: return "downloading";
    case Phase::NewCharacter:
        if (s.PreparingFirstSave) return "saving-first-checkpoint";
        return s.NativeEnabled ? "new-native" : "new";
    case Phase::Staged: return s.NativeEnabled ? "awaiting-native-load" : "staged";
    case Phase::LoadingNative: return "loading-native";
    case Phase::Ready: return "ready";
    case Phase::Upload: return "uploading";
    default: return "failed";
    }
}
// Game.psc declares QuitToMainMenu as a global native with zero arguments.
bool RequestMainMenu() noexcept
{
    auto* vm = GameVM::Get();
    if (!vm || !vm->virtualMachine) return false;
    struct EmptyArguments final : BSScript::IFunctionArguments
    {
        bool Prepare(Statement* apOutput) const noexcept override
        {
            apOutput->SetSize(0);
            return true;
        }
    };
    static EmptyArguments args;
    static const BSFixedString script("Game"), function("QuitToMainMenu");
    BSTSmartPointer<BSScript::IStackCallbackFunctor> callback;
    return vm->virtualMachine->DispatchStaticCall(script, function, &args, callback);
}

void Change(Phase aPhase, const char* apDetail)
{
    s.Step = aPhase;
    s.Detail = apDetail;
    PublishState();
}
void AbandonNative() noexcept
{
    s.LobbySaveLoaded = false;
    MainMenuBridge::CancelNewGame();
    if (s.NativeToken && s_driver.Abandon)
        s_driver.Abandon(s.NativeToken);
    s.NativeToken = 0;
    s.Operation = NativeOperation::None;
    s.CaptureReady = false;
    s.AutoEnter = false;
}
void Fail(const char* apDetail)
{
    AbandonNative();
    s.Pending = false;
    s.Flights.clear();
    s.Bytes.clear();
    Change(Phase::Failed, apDetail);
    spdlog::warn("Character checkpoint: {}", apDetail);
}

std::filesystem::path SaveDirectory(bool aGog = false)
{
    // Mod managers can redirect saves. The override must name the actual
    // native save directory; guessing a profile is never safe.
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"ST_SAVE_DIRECTORY", path, static_cast<DWORD>(std::size(path)));
    if (length && length < std::size(path))
        return std::filesystem::path(path);
    if (length)
        return {};
    wchar_t documents[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents)))
        return {};
    const auto base = std::filesystem::path(documents) / L"My Games" /
        (aGog ? L"Skyrim Special Edition GOG" : L"Skyrim Special Edition");
    auto* settings = INISettingCollection::Get();
    const auto* setting = settings ? settings->GetSetting("sLocalSavePath:General") : nullptr;
    const auto* configuredPath = setting ? reinterpret_cast<const char*>(setting->data) : nullptr;
    return base / (configuredPath && *configuredPath ? configuredPath : "Saves");
}
bool PrepareLocalLobbySave() noexcept
{
    try
    {
        const auto directory = SaveDirectory();
        if (directory.empty() || !directory.is_absolute()) return false;
        const auto hasPair = [](const std::filesystem::path& folder)
        {
            const auto validHeader = [&](const char* extension, std::string_view expected)
            {
                std::ifstream file(folder / (std::string(STCheckpointBridge::LobbySaveName) + extension), std::ios::binary);
                std::array<char, 13> header{};
                return file.read(header.data(), expected.size()) &&
                    std::string_view(header.data(), expected.size()) == expected;
            };
            return validHeader(".ess", "TESV_SAVEGAME") && validHeader(".skse", "SKSE");
        };
        if (hasPair(directory)) return true;
        if (GetEnvironmentVariableW(L"ST_SAVE_DIRECTORY", nullptr, 0) == 0 &&
            hasPair(SaveDirectory(true)))
            return true;

        const auto source = TiltedPhoques::GetPath() / "LobbySave";
        if (!hasPair(source))
        {
            spdlog::warn(L"Bundled lobby save pair is missing or unreadable in {}", source.wstring());
            return false;
        }
        const std::string stem(STCheckpointBridge::LobbySaveName);
        const auto ess = directory / (stem + ".ess");
        const auto coSave = directory / (stem + ".skse");
        std::error_code ec;
        if (std::filesystem::exists(ess, ec) || ec || std::filesystem::exists(coSave, ec) || ec)
        {
            spdlog::warn(L"Cannot install the bundled lobby save over existing or inaccessible files in {}", directory.wstring());
            return false;
        }
        std::filesystem::create_directories(directory, ec);
        if (ec || !std::filesystem::copy_file(source / coSave.filename(), coSave, ec))
        {
            spdlog::warn(L"Could not install the lobby co-save in {} (filesystem error {})", directory.wstring(), ec.value());
            return false;
        }
        if (!std::filesystem::copy_file(source / ess.filename(), ess, ec))
        {
            spdlog::warn(L"Could not install the lobby save in {} (filesystem error {})", directory.wstring(), ec.value());
            std::filesystem::remove(coSave, ec);
            return false;
        }
        spdlog::info("Installed the bundled Abandoned Prison lobby save pair");
        return true;
    }
    catch (...) { return false; }
}

std::filesystem::path Path(const char* apExtension)
{
    return s.Directory / (s.Name + apExtension);
}
bool ChoosePath()
{
    s.Directory = SaveDirectory();
    if (s.Directory.empty() || !s.Directory.is_absolute()) return false;
    std::error_code ec;
    std::filesystem::create_directories(s.Directory, ec);
    if (ec) return false;
    // Random, session-specific names prevent accidentally overwriting an
    // offline save or another instance's cached checkpoint.
    std::array<uint8_t, 12> random{};
    CryptoPP::AutoSeededRandomPool().GenerateBlock(random.data(), random.size());
    s.Name = "STCampaign_";
    for (const auto byte : random)
    {
        s.Name += kHex[byte >> 4];
        s.Name += kHex[byte & 15];
    }
    return !std::filesystem::exists(Path(".ess"), ec) && !ec &&
           !std::filesystem::exists(Path(".skse"), ec) && !ec;
}
struct ReadHandle
{
    HANDLE Value{INVALID_HANDLE_VALUE};
    ~ReadHandle() { if (Value != INVALID_HANDLE_VALUE) CloseHandle(Value); }
    bool Open(const std::filesystem::path& acPath)
    {
        // Exclude writers and renames throughout the snapshot.
        Value = CreateFileW(acPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        return Value != INVALID_HANDLE_VALUE;
    }
    bool Stat(Stamp& aStamp) const
    {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(Value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return false;
        aStamp.Size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        aStamp.Written = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
        return true;
    }
    bool Read(uint8_t* apData, uint64_t aSize) const
    {
        while (aSize)
        {
            const DWORD length = static_cast<DWORD>(std::min<uint64_t>(aSize, 65536));
            DWORD actual{};
            if (!ReadFile(Value, apData, length, &actual, nullptr) || actual != length) return false;
            apData += actual;
            aSize -= actual;
        }
        return true;
    }
};
bool Snapshot(PairStamp& aStamp, bool aRead)
{
    ReadHandle ess, co;
    if (!ess.Open(Path(".ess")) || !ess.Stat(aStamp.Ess)) return false;
    const auto coPath = Path(".skse");
    std::error_code ec;
    const bool coExists = std::filesystem::exists(coPath, ec);
    if (ec || ((IsScriptExtenderLoaded() || coExists) && (!co.Open(coPath) || !co.Stat(aStamp.Co)))) return false;
    if (aStamp.Ess.Size < 13 || aStamp.Ess.Size > P::MaxSize ||
        aStamp.Co.Size > P::MaxSize - aStamp.Ess.Size || (IsScriptExtenderLoaded() && !aStamp.Co.Size))
        return false;
    if (!aRead) return true;
    s.Bytes.resize(static_cast<size_t>(aStamp.Ess.Size + aStamp.Co.Size));
    return ess.Read(s.Bytes.data(), aStamp.Ess.Size) &&
        (!aStamp.Co.Size || co.Read(s.Bytes.data() + aStamp.Ess.Size, aStamp.Co.Size));
}
bool WriteNew(const std::filesystem::path& acPath, const uint8_t* apData, uint64_t aSize)
{
    const HANDLE file = CreateFileW(acPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    while (aSize && ok)
    {
        const DWORD length = static_cast<DWORD>(std::min<uint64_t>(aSize, 65536));
        DWORD actual{};
        ok = WriteFile(file, apData, length, &actual, nullptr) && actual == length;
        if (!ok) break;
        apData += actual;
        aSize -= actual;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!ok) DeleteFileW(acPath.c_str());
    return ok;
}
std::array<uint8_t, 32> BlockDigest(size_t aOffset, size_t aCount)
{
    std::array<uint8_t, 32> digest{};
    CryptoPP::SHA256().CalculateDigest(digest.data(), s.Bytes.data() + aOffset, aCount);
    return digest;
}

void ReadCachedCheckpoint()
{
    ReadHandle file;
    Stamp stamp{};
    if (!file.Open(s.CacheFile) || !file.Stat(stamp) || stamp.Size > P::MaxSize) return;
    const size_t count = std::min(s.Bytes.size(), static_cast<size_t>(stamp.Size));
    if (file.Read(s.Bytes.data(), count)) s.CachedSize = count;
}

void RememberCheckpoint()
{
    // BaseBlocks describes only the verified server revision for this selection.
    s.BaseBlocks.clear();
    for (size_t offset = 0; offset < s.Bytes.size(); offset += P::ChunkSize)
        s.BaseBlocks.push_back(BlockDigest(offset, std::min(P::ChunkSize, s.Bytes.size() - offset)));

    // This cache is optional. A unique temporary file keeps concurrent clients and
    // interrupted writes from replacing a usable cache with a partial checkpoint.
    std::error_code ec;
    std::filesystem::create_directories(s.CacheFile.parent_path(), ec);
    const auto temp = s.CacheFile.parent_path() / (s.Name + ".tmp");
    if (ec || !WriteNew(temp, s.Bytes.data(), s.Bytes.size()) ||
        !MoveFileExW(temp.c_str(), s.CacheFile.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(temp.c_str());
        spdlog::warn("Could not refresh the local character checkpoint cache");
    }
}

bool StageDownload()
{
    std::array<uint8_t, 32> digest{};
    CryptoPP::SHA256().CalculateDigest(digest.data(), s.Bytes.data(), s.Bytes.size());
    if (digest != s.Digest || s.Bytes.size() < 13 ||
        std::string_view(reinterpret_cast<const char*>(s.Bytes.data()), 13) != "TESV_SAVEGAME")
        return false;
    if (IsScriptExtenderLoaded() && !s.Co) return false;
    const auto essPending = Path(".ess.pending");
    const auto coPending = Path(".skse.pending");
    if (!WriteNew(essPending, s.Bytes.data(), s.Ess)) return false;
    if (s.Co && !WriteNew(coPending, s.Bytes.data() + s.Ess, s.Co))
    {
        DeleteFileW(essPending.c_str());
        return false;
    }
    // Publish co-save first, ESS last. Skyrim never sees the ESS until
    // both payloads were completely written and verified.
    const bool coPublished = !s.Co || MoveFileExW(coPending.c_str(), Path(".skse").c_str(), MOVEFILE_WRITE_THROUGH);
    if (!coPublished || !MoveFileExW(essPending.c_str(), Path(".ess").c_str(), MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(essPending.c_str());
        if (s.Co) DeleteFileW(coPending.c_str());
        if (s.Co && coPublished) DeleteFileW(Path(".skse").c_str());
        return false;
    }
    PairStamp baseline{};
    if (!Snapshot(baseline, false)) return false;
    s.Baseline = baseline;
    RememberCheckpoint();
    s.Bytes.clear();
    return true;
}

NativeStart StartNative(NativeOperation aOperation)
{
    const bool lobby = aOperation == NativeOperation::LobbyLoad;
    if (!s.NativeEnabled || s.Operation != NativeOperation::None || (!lobby && s.Name.empty()))
        return NativeStart::Busy;
    if (++s_nextNativeToken == 0) ++s_nextNativeToken;
    const uint64_t token = s_nextNativeToken;
    const auto result = aOperation == NativeOperation::Save
        ? s_driver.RequestSave(token, s.Name.c_str())
        : s_driver.RequestLoad(token, lobby ? STCheckpointBridge::LobbySaveName : s.Name.c_str());
    if (result == NativeStart::Accepted)
    {
        s.Operation = aOperation;
        s.NativeToken = token;
        s.NativeDeadline = Clock::now() + kNativeTimeout;
        if (lobby)
            Change(Phase::LoadingLobby, "Loading the local Abandoned Prison lobby...");
        else if (aOperation == NativeOperation::Load)
            Change(Phase::LoadingNative, "Loading the canonical server checkpoint...");
        else
        {
            s.CaptureReady = false;
            s.StableSince = {};
            s.Detail = "Creating a native checkpoint...";
            PublishState();
        }
    }
    return result;
}

void ResetTransfer()
{
    s.Offset = s.SentOffset = 0;
    s.Flights.clear();
    s.TransferStart = Clock::now();
    s.LastProgress = {};
}
void UpdateTransferProgress()
{
    if (s.Bytes.empty()) return;
    const auto now = Clock::now();
    if (s.Offset != s.Bytes.size() && now - s.LastProgress < std::chrono::milliseconds(250)) return;
    s.LastProgress = now;
    const double seconds = (std::max)(0.001, std::chrono::duration<double>(now - s.TransferStart).count());
    s.Detail = std::string(s.Step == Phase::Upload ? "Uploading checkpoint: " : "Downloading checkpoint: ") +
        std::to_string(100 * s.Offset / s.Bytes.size()) + "% (" +
        std::to_string(static_cast<uint64_t>(s.Offset / seconds / 1024)) + " KiB/s effective)";
    if (s.Step == Phase::Upload && s.Offset == s.Bytes.size()) s.Detail += "; verifying and committing...";
    PublishState();
}
bool SendFlight(TransportService& aTransport, const RequestCharacterSave& acRequest, uint64_t aEnd)
{
    if (!aTransport.Send(acRequest))
    {
        Fail("Connection unavailable; reconnect to resume.");
        return false;
    }
    if (s.Flights.empty()) s.Deadline = Clock::now() + std::chrono::seconds(30);
    s.Flights.push_back({acRequest.Action, aEnd});
    s.SentOffset = aEnd;
    return true;
}

bool Send(TransportService& aTransport, const RequestCharacterSave& acRequest)
{
    if (!aTransport.Send(acRequest))
    {
        Fail("Connection unavailable; reconnect to resume.");
        return false;
    }
    s.Pending = true;
    s.Expected = acRequest.Action;
    s.Deadline = Clock::now() + std::chrono::seconds(30);
    return true;
}
}

void PublishState() noexcept
{
    auto* overlay = World::Get().GetOverlayService().GetOverlayApp();
    if (!overlay) return;
    auto args = CefListValue::Create();
    args->SetBool(0, s.Required);
    args->SetString(1, PhaseName());
    args->SetString(2, s.Name);
    args->SetString(3, std::to_string(s.Revision));
    args->SetString(4, s.Detail);
    args->SetString(5, s.Step == Phase::Choose && s.HasRoster ? s.Roster : "");
    overlay->ExecuteAsync("characterCheckpoint", args);
}
bool RegisterNativeDriver(const NativeDriver& acDriver) noexcept
{
    if (s.Required || s_driver.RequestLoad || !acDriver.RequestLoad ||
        !acDriver.RequestSave || !acDriver.Poll || !acDriver.Abandon)
        return false;
    s_driver = acDriver;
    return true;
}

bool FinishCharacter(TransportService& aTransport) noexcept
{
    if (!s.NativeEnabled || s.Step != Phase::NewCharacter)
        return false;
    if (!s.SawRaceMenu || !aTransport.IsInLobby() || !aTransport.CanEnterWorld() || s.Pending)
        return true;
    if (s.Operation != NativeOperation::None || s.CaptureReady) return true;
    const auto result = StartNative(NativeOperation::Save);
    if (result == NativeStart::Accepted)
        s.AutoEnter = true; // only after the initial checkpoint is committed
    else if (result == NativeStart::Busy)
    {
        s.NextPoll = Clock::now() + std::chrono::seconds(5);
    }
    else
        Fail("Skyrim rejected the initial save request.");
    return true;
}

bool IsAlternateStartPickerEnabled() noexcept
{
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"ST_ALTERNATE_START_PICKER", value, 2) != 1 || value[0] != L'0';
}

bool CanReplaceStagingWorld() noexcept
{
    if (!s.LobbySaveLoaded ||
        NativeCheckpointDriver::NewGameGeneration() != s.InitialNewGame ||
        NativeCheckpointDriver::LoadGeneration() - s.InitialLoadGeneration != 1)
        return false;
    const auto* player = PlayerCharacter::Get();
    const auto* ui = UI::Get();
    return player && player->parentCell && player->parentCell->formID == kAbandonedPrison && ui &&
        !ui->GetMenuOpen("RaceSex Menu") && !ui->GetMenuOpen("Loading Menu");
}

bool NeedsStagingPump() noexcept
{
    return s.Step == Phase::LoadingLobby || s.Step == Phase::WaitingForNewGame || s.LobbySaveLoaded ||
        (s.NativeEnabled && (s.Step == Phase::NewCharacter || s.Step == Phase::Upload));
}

void Begin(bool aRequired) noexcept
{
    AbandonNative();
    s = {};
    if (aRequired) NativeCheckpointDriver::TryRegister();
    s.Required = aRequired;
    s.NativeEnabled = aRequired && s_driver.RequestLoad != nullptr;
    if (s.NativeEnabled && IsAlternateStartPickerEnabled() && MainMenuBridge::IsReady())
    {
        const auto* mods = ModManager::Get();
        const auto* mod = mods ? mods->GetByName("Alternate Start - Live Another Life.esp") : nullptr;
        if (mod && mod->IsLoaded() && !mod->IsLite())
        {
            if (PrepareLocalLobbySave())
            {
                s.InitialNewGame = NativeCheckpointDriver::NewGameGeneration();
                s.InitialLoadGeneration = NativeCheckpointDriver::LoadGeneration();
                if (StartNative(NativeOperation::LobbyLoad) == NativeStart::Accepted)
                {
                    spdlog::info("Queued the local Abandoned Prison lobby save");
                    return;
                }
                spdlog::warn("Could not queue the local lobby save; using the title-screen picker");
            }
            else
                spdlog::info("Local lobby save pair not found or unreadable; using the title-screen picker");
        }
    }
    Change(aRequired ? Phase::Choose : Phase::Disabled, aRequired ? (s.NativeEnabled ? "Choose a character slot." : "Choose a character slot. Native automation unavailable; manual save/load required.") : "");
}
void Reset() noexcept
{
    if (!s.Revision && s.PreparingFirstSave)
        spdlog::warn("Disconnected before the first server checkpoint was confirmed");
    AbandonNative();
    s = {};
    PublishState();
}
bool CanEnter() noexcept
{
    if (s.PreparingFirstSave && s.Revision && s.NativeEnabled &&
        NativeCheckpointDriver::LoadGeneration() - s.InitialLoadGeneration != 1)
        return false;
    return !s.Required || ((s.Step == Phase::Ready || (!s.NativeEnabled && s.Step == Phase::Staged)) && !s.Pending && s.Revision);
}

static bool DecodeKey(const std::string& text, std::array<uint8_t, 32>& key) noexcept
{
    if (text.size() != 64) return false;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const auto hi = std::string_view(kHex, 16).find(text[2 * i]);
        const auto lo = std::string_view(kHex, 16).find(text[2 * i + 1]);
        if (hi == std::string_view::npos || lo == std::string_view::npos) return false;
        key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void RequestRoster(TransportService& transport, const std::string& owner, const std::string& legacy) noexcept
{
    if (!transport.IsInLobby() || s.Step != Phase::Choose || s.HasRoster || s.Pending) return;
    RequestCharacterSave request{};
    if (!DecodeKey(owner, request.Key) || legacy.size() > 32 * (64 + 104 + 1))
    {
        Fail("Could not access the local identity for this server.");
        return;
    }
    request.Action = P::Roster;
    request.Count = static_cast<uint16_t>(legacy.size());
    std::copy(legacy.begin(), legacy.end(), request.Data.begin());
    Send(transport, request);
}

void Select(TransportService& aTransport, const std::string& acKey, const std::string& acLabel, bool aCreate) noexcept
{
    if (!aTransport.IsInLobby() || s.Step != Phase::Choose || !s.HasRoster || s.Pending) return;
    if (s.LobbySaveLoaded && !CanReplaceStagingWorld())
    {
        Fail("The temporary character lobby was interrupted. Return to the main menu and reconnect.");
        return;
    }
    try
    {
        RequestCharacterSave request{};
        request.Action = aCreate ? P::Create : P::SelectOwned;
        if (!DecodeKey(acKey, request.Key) || acLabel.empty() || acLabel.size() > 104) return;
        if (aCreate)
        {
            request.Count = static_cast<uint16_t>(acLabel.size());
            std::copy(acLabel.begin(), acLabel.end(), request.Data.begin());
        }
        if (!ChoosePath()) { Change(Phase::Choose, "Save directory unavailable; configure ST_SAVE_DIRECTORY."); return; }
        std::array<uint8_t, 32> slotHash{};
        CryptoPP::SHA256().CalculateDigest(slotHash.data(), request.Key.data(), request.Key.size());
        std::string cacheName;
        for (const auto byte : slotHash)
        {
            cacheName += kHex[byte >> 4];
            cacheName += kHex[byte & 15];
        }
        if (!aCreate) cacheName = acKey;
        s.CacheFile = s.Directory / L"SkyrimTogetherCache" / (cacheName + ".checkpoint");
        s.CharacterLabel = acLabel;
        s.NextNameAttempt = {};
        Change(Phase::Selecting, "Checking server character...");
        Send(aTransport, request);
    }
    catch (...) { Fail("Could not prepare a character save directory."); }
}

void Receive(const NotifyCharacterSave& acReply) noexcept
{
    if (!s.Required) return;
    Flight completed{};
    if (s.Pending)
    {
        if (acReply.Action != s.Expected) return;
        s.Pending = false;
    }
    else if (!s.Flights.empty())
    {
        if (acReply.Action != s.Flights.front().Action) { Fail("Unexpected checkpoint response."); return; }
        completed = s.Flights.front();
        s.Flights.erase(s.Flights.begin());
        s.Deadline = Clock::now() + std::chrono::seconds(30);
    }
    else return;
    try
    {
        if (acReply.Status != P::Ok)
        {
            if (acReply.Action == P::Roster) { Fail("Could not read the server character list. Reconnect to retry."); return; }
            if (s.Step == Phase::Selecting)
            {
                if (acReply.Status == P::Missing) s.HasRoster = false;
                Change(Phase::Choose, acReply.Status == P::Busy ? "Character is already in use." : "Server could not select the character.");
                return;
            }
            Fail("Server rejected checkpoint transfer; reconnect to resume.");
            return;
        }
        switch (acReply.Action)
        {
        case P::Roster:
        {
            s.Roster.assign(reinterpret_cast<const char*>(acReply.Data.data()), acReply.Count);
            s.HasRoster = true;
            Change(Phase::Choose, "Choose a character to enter the campaign.");
            return;
        }
        case P::Create:
        case P::SelectOwned:
            s.Revision = acReply.Revision;
            if (!s.Revision)
            {
                if (s.NativeEnabled)
                {
                    if (CanReplaceStagingWorld())
                    {
                        Change(Phase::WaitingForNewGame, "Returning to Skyrim's main menu to start character creation...");
                        s.NewGameDeadline = Clock::now() + kNativeTimeout;
                        World::Get().QueueCheckpointTask([selection = s.Name]()
                        {
                            if (s.Step != Phase::WaitingForNewGame || s.Name != selection ||
                                !World::Get().GetTransport().IsInLobby() || !CanReplaceStagingWorld()) return;
                            if (RequestMainMenu())
                            {
                                spdlog::info("Requested return to the main menu for character creation");
                                InputService::SetOverlayActive(World::Get().GetOverlayService(), false);
                            }
                            else
                            {
                                s.NewGameDeadline = {};
                                Change(Phase::WaitingForNewGame, "Could not return automatically. Return to Skyrim's main menu to start character creation.");
                            }
                        });
                        return;
                    }
                    auto* ui = UI::Get();
                    if (!ui || !ui->GetMenuOpen("Main Menu"))
                    {
                        Fail("Create a new server character from Skyrim's main menu.");
                        return;
                    }
                    s.InitialNewGame = NativeCheckpointDriver::NewGameGeneration();
                    s.InitialLoadGeneration = NativeCheckpointDriver::LoadGeneration();
                    s.AwaitCharacterCreation = true;
                    if (MainMenuBridge::IsReady())
                    {
                        Change(Phase::NewCharacter, "Starting Skyrim's native character creation. Your character is saved automatically after RaceMenu.");
                        if (!InputService::SetOverlayActive(World::Get().GetOverlayService(), false) ||
                            !MainMenuBridge::RequestNewGame())
                        {
                            Fail("Could not start a native new game. Reconnect from the main menu.");
                            return;
                        }
                        s.RequestedNewGame = true;
                        s.NewGameDeadline = Clock::now() + kNativeTimeout;
                    }
                    else
                    {
                        Change(Phase::NewCharacter, "Choose New Game in Skyrim. Your character will be saved automatically after RaceMenu.");
                        InputService::SetOverlayActive(World::Get().GetOverlayService(), false);
                    }
                }
                else if (MainMenuBridge::IsReady())
                    Fail("The SKSE checkpoint bridge is required for the Play-only menu. Install the matching bridge or restore your original menu.");
                else
                    Change(Phase::NewCharacter, "Create your character in Skyrim, then make the named checkpoint save. It uploads automatically.");
                return;
            }
            if (acReply.EssSize < 13 || acReply.EssSize > P::MaxSize ||
                acReply.CoSaveSize > P::MaxSize - acReply.EssSize) { Fail("Invalid save size."); return; }
            s.Ess = acReply.EssSize;
            s.Co = acReply.CoSaveSize;
            s.Digest = acReply.Digest;
            ResetTransfer();
            s.Bytes.resize(static_cast<size_t>(s.Ess + s.Co));
            ReadCachedCheckpoint();
            Change(Phase::Download, "Downloading the server's latest checkpoint...");
            return;
        case P::DownloadIfChanged:
        case P::DownloadChunk:
        {
            const size_t count = std::min<uint64_t>(P::ChunkSize, s.Bytes.size() - s.Offset);
            const bool reuse = acReply.Action == P::DownloadIfChanged && !acReply.Count;
            if (s.Step != Phase::Download || acReply.Revision != s.Revision ||
                acReply.EssSize != s.Ess || acReply.CoSaveSize != s.Co || acReply.Digest != s.Digest ||
                acReply.Offset != s.Offset || !count || s.Offset + count != completed.End ||
                (reuse ? s.Offset + count > s.CachedSize : acReply.Count != count))
            { Fail("Invalid download response."); return; }
            if (reuse)
                s.ReusedBytes += count;
            else
                std::copy_n(acReply.Data.begin(), count, s.Bytes.begin() + static_cast<size_t>(s.Offset));
            s.Offset += count;
            UpdateTransferProgress();
            if (s.Offset == s.Bytes.size())
            {
                if (!StageDownload()) { Fail("Checkpoint failed integrity verification or could not be staged."); return; }
                spdlog::info("Checkpoint download reused {} of {} bytes", s.ReusedBytes, s.Offset);
                Change(Phase::Staged, s.NativeEnabled ? "Download verified. Waiting for Skyrim to load the selected checkpoint..." : "Downloaded. Load the named save in Skyrim, then select Enter world.");
                s.EnterDeadline = Clock::now() + kNativeTimeout;
            }
            return;
        }
        case P::BeginUpload:
            if (s.Step != Phase::Upload || acReply.Revision != s.Revision) { Fail("Invalid upload response."); return; }
            s.NeedsBegin = false;
            s.UploadRateKiB = static_cast<uint32_t>(std::clamp<uint64_t>(acReply.Offset, P::MinRateKiB, P::MaxRateKiB));
            s.TransferStart = s.CreditTime = Clock::now();
            s.UploadCredit = P::ChunkSize + P::HeaderSize;
            {
                const auto status = World::Get().GetTransport().GetConnectionStatus();
                spdlog::info("Checkpoint upload: configured {} KiB/s, estimated socket rate {} KiB/s, ping {} ms",
                    s.UploadRateKiB, status.m_nSendRateBytesPerSecond / 1024, status.m_nPing);
            }
            return;
        case P::UploadCopy:
        case P::UploadChunk:
        case P::CompressedUploadChunk:
            if (s.Step != Phase::Upload || acReply.Revision != s.Revision ||
                acReply.Offset != completed.End || acReply.Offset <= s.Offset || acReply.Offset > s.SentOffset)
                { Fail("Invalid upload offset."); return; }
            s.Offset = acReply.Offset;
            UpdateTransferProgress();
            return;
        case P::Commit:
            if (s.Step != Phase::Upload || acReply.Revision != s.Revision + 1 || acReply.Digest != s.Digest ||
                acReply.EssSize != s.Ess || acReply.CoSaveSize != s.Co) { Fail("Invalid commit response."); return; }
            if (!s.Revision)
            {
                spdlog::info("First character checkpoint committed by the server (revision {})", acReply.Revision);
                if (const auto* ui = UI::Get(); ui && ui->GetMenuOpen("Main Menu"))
                    s.AutoEnter = false;
            }
            s.Revision = acReply.Revision;
            s.Baseline = s.Captured;
            spdlog::info("Checkpoint upload reused {} of {} bytes", s.ReusedBytes, s.Bytes.size());
            RememberCheckpoint();
            s.Bytes.clear();
            s.CaptureReady = false;
            s.NextAutoSave = Clock::now() + kAutosaveInterval;
            if (s.AutoEnter) s.EnterDeadline = Clock::now() + kNativeTimeout;
            Change(Phase::Ready, "Checkpoint saved on the server.");
            return;
        default: Fail("Unexpected checkpoint response."); return;
        }
    }
    catch (...) { Fail("Unable to complete checkpoint transfer."); }
}

void Tick(TransportService& aTransport) noexcept
{
    if (s.Required && s.NativeEnabled) NativeCheckpointDriver::CheckForUnexpectedLoad();
    if (!s.Required || (!aTransport.IsInLobby() && !aTransport.IsOnline())) return;
    try
    {
        const auto now = Clock::now();
        if (s.Step == Phase::WaitingForNewGame)
        {
            const auto* ui = UI::Get();
            if (!ui || !ui->GetMenuOpen("Main Menu") || !MainMenuBridge::IsReady())
            {
                if (s.NewGameDeadline != Clock::time_point{} && now >= s.NewGameDeadline)
                {
                    s.NewGameDeadline = {};
                    Change(Phase::WaitingForNewGame, "Still waiting for Skyrim. Return to the main menu to start character creation.");
                    InputService::SetOverlayActive(World::Get().GetOverlayService(), true);
                }
                return;
            }
            s.LobbySaveLoaded = false;
            s.InitialNewGame = NativeCheckpointDriver::NewGameGeneration();
            s.InitialLoadGeneration = NativeCheckpointDriver::LoadGeneration();
            if (!MainMenuBridge::RequestNewGame())
            {
                Fail("Could not start a fresh game for the selected character.");
                return;
            }
            s.AwaitCharacterCreation = true;
            s.RequestedNewGame = true;
            s.NewGameDeadline = now + kNativeTimeout;
            InputService::SetOverlayActive(World::Get().GetOverlayService(), false);
            Change(Phase::NewCharacter, "Starting a fresh game for your new character...");
            return;
        }
        if (s.Step == Phase::LoadingLobby && s.LobbySaveLoaded)
        {
            auto& overlay = World::Get().GetOverlayService();
            if (CanReplaceStagingWorld() && aTransport.CanEnterWorld() && overlay.GetInGame() &&
                InputService::SetOverlayActive(overlay, true))
            {
                Change(Phase::Choose, "Choose a character to enter the campaign.");
                spdlog::info("Opened the character picker from the local lobby save");
            }
            else if (now >= s.NativeDeadline)
                Fail("The local lobby loaded but did not reach the Abandoned Prison.");
            return;
        }
        if (s.Pending)
        {
            if (now >= s.Deadline) Fail("Checkpoint transfer timed out; reconnect to resume.");
            return;
        }
        if (!s.Flights.empty() && now >= s.Deadline)
        {
            Fail("Checkpoint transfer timed out; reconnect to resume.");
            return;
        }
        if (s.NativeEnabled && s.Operation != NativeOperation::None)
        {
            const auto result = s_driver.Poll(s.NativeToken);
            if (result == NativeResult::Pending)
            {
                if (now >= s.NativeDeadline) Fail("Native save/load timed out; reconnect to resume.");
                return;
            }
            const auto completed = s.Operation;
            s.NativeToken = 0;
            s.Operation = NativeOperation::None;
            if (result != NativeResult::Succeeded)
            {
                Fail("Skyrim could not complete the named save/load.");
                return;
            }
            if (completed == NativeOperation::LobbyLoad)
            {
                s.LobbySaveLoaded = true;
                s.NativeDeadline = now + kNativeTimeout;
                return;
            }
            if (completed == NativeOperation::Load)
            {
                s.LobbySaveLoaded = false;
                s.AutoEnter = true;
                s.EnterDeadline = now + kNativeTimeout;
                s.NextAutoSave = now + kAutosaveInterval;
                Change(Phase::Ready, "Canonical checkpoint loaded. Entering server...");
                return;
            }
            s.CaptureReady = true;
            s.NativeDeadline = now + kNativeTimeout;
        }

        if (s.NativeEnabled && s.Step == Phase::NewCharacter && s.AwaitCharacterCreation &&
            !s.CaptureReady && s.Operation == NativeOperation::None)
        {
            const auto newGames = NativeCheckpointDriver::NewGameGeneration() - s.InitialNewGame;
            const auto loads = NativeCheckpointDriver::LoadGeneration() - s.InitialLoadGeneration;
            if (s.RequestedNewGame && newGames == 0 &&
                (MainMenuBridge::NewGameRequestFailed() || now >= s.NewGameDeadline))
            {
                Fail("Native New Game did not complete. Reconnect or restore your original menu.");
                return;
            }
            if (newGames == 1 && s.RequestedNewGame)
            {
                MainMenuBridge::CancelNewGame();
                s.RequestedNewGame = false;
            }
            if (loads != newGames || newGames > 1)
            {
                Fail("An unrelated save or new game was opened during character creation.");
                return;
            }
            auto* ui = UI::Get();
            if (newGames == 1 && ui && ui->GetMenuOpen("RaceSex Menu"))
            {
                if (!s.CharacterLabel.empty() && now >= s.NextNameAttempt)
                {
                    s.NextNameAttempt = now + std::chrono::seconds(1);
                    const auto generation = s.InitialNewGame;
                    World::Get().QueueCheckpointTask([generation]()
                    {
                        if (s.Step == Phase::NewCharacter && s.AwaitCharacterCreation &&
                            s.InitialNewGame == generation && NativeCheckpointDriver::NewGameGeneration() - generation == 1 &&
                            !s.CharacterLabel.empty() && MainMenuBridge::PrefillCharacterName(s.CharacterLabel.c_str()))
                            s.CharacterLabel.clear();
                    });
                }
                s.SawRaceMenu = true;
                s.CharacterReadySince = {};
                return;
            }
            if (newGames == 1 && s.SawRaceMenu && ui && !s.PreparingFirstSave)
            {
                s.PreparingFirstSave = true;
                Change(Phase::NewCharacter, "Preparing your first server save. Stay in game until it finishes.");
            }
            if (newGames != 1 || !s.SawRaceMenu || !aTransport.CanEnterWorld())
            {
                s.CharacterReadySince = {};
                return;
            }
            if (!s.CharacterLabel.empty())
            {
                spdlog::warn("RaceMenu closed before the campaign name could be prefilled");
                s.CharacterLabel.clear();
            }
            if (s.CharacterReadySince == Clock::time_point{})
                s.CharacterReadySince = now;
            if (now - s.CharacterReadySince >= std::chrono::seconds(5) && now >= s.NextPoll)
                FinishCharacter(aTransport);
            return;
        }
        if (s.NativeEnabled && s.Step == Phase::Staged)
        {
            if (now >= s.EnterDeadline) { Fail("Skyrim did not become ready for the checkpoint load."); return; }
            if (StartNative(NativeOperation::Load) == NativeStart::Failed)
                Fail("Skyrim rejected the canonical checkpoint load.");
            return;
        }
        if (s.NativeEnabled && s.AutoEnter && s.Step == Phase::Ready && aTransport.IsInLobby())
        {
            if (aTransport.CanEnterWorld() && aTransport.EnterWorld())
                s.AutoEnter = false;
            else if (now >= s.EnterDeadline)
                Fail("World entry did not become ready.");
            return;
        }
        if (s.NativeEnabled && s.Step == Phase::Ready && aTransport.IsOnline() &&
            !s.CaptureReady && now >= s.NextAutoSave && aTransport.CanEnterWorld())
        {
            const auto result = StartNative(NativeOperation::Save);
            s.NextAutoSave = now + (result == NativeStart::Busy ? std::chrono::seconds(5) : kAutosaveInterval);
            if (result == NativeStart::Failed) Fail("Skyrim rejected the autosave.");
            return;
        }

        if (s.Step == Phase::Download || s.Step == Phase::Upload)
        {
            const bool uploading = s.Step == Phase::Upload;
            if (uploading && s.NeedsBegin)
            {
                RequestCharacterSave request{};
                request.Revision = s.Revision;
                request.Action = P::BeginUpload;
                request.EssSize = s.Ess;
                request.CoSaveSize = s.Co;
                request.Digest = s.Digest;
                Send(aTransport, request);
                return;
            }
            const bool online = aTransport.IsOnline();
            const size_t budget = online ? 2 : 8;
            const size_t window = online ? 4 : P::WindowSize;
            if (uploading)
            {
                const double elapsed = std::chrono::duration<double>(now - s.CreditTime).count();
                s.CreditTime = now;
                s.UploadCredit = (std::min)(static_cast<double>(P::WindowSize * (P::ChunkSize + P::HeaderSize)),
                    s.UploadCredit + (std::max)(0.0, elapsed) * s.UploadRateKiB * 1024.0);
            }
            for (size_t i = 0; i < budget && s.Flights.size() < window && s.SentOffset < s.Bytes.size(); ++i)
            {
                // Do not build a bulk reliable backlog while gameplay is active.
                const auto status = aTransport.GetConnectionStatus();
                const auto pending = static_cast<int64_t>(status.m_cbPendingReliable) + status.m_cbPendingUnreliable;
                if (pending >= (online ? 64 * 1024 : 2 * 1024 * 1024)) break;
                RequestCharacterSave request{};
                request.Revision = s.Revision;
                request.Offset = s.SentOffset;
                const size_t count = std::min<uint64_t>(P::ChunkSize, s.Bytes.size() - s.SentOffset);
                if (!uploading)
                {
                    request.Action = P::DownloadChunk;
                    if (s.SentOffset + count <= s.CachedSize)
                    {
                        request.Action = P::DownloadIfChanged;
                        request.Digest = BlockDigest(s.SentOffset, count);
                    }
                }
                else
                {
                    const size_t block = s.SentOffset / P::ChunkSize;
                    if (block < s.BaseBlocks.size() && BlockDigest(s.SentOffset, count) == s.BaseBlocks[block])
                    {
                        request.Action = P::UploadCopy;
                        // Account for reuse only after successfully scheduling the request.
                    }
                    else
                    {
                        const auto* bytes = s.Bytes.data() + static_cast<size_t>(s.SentOffset);
                        uLongf size = static_cast<uLongf>(request.Data.size());
                        if (compress2(request.Data.data(), &size, bytes, static_cast<uLong>(count), Z_BEST_SPEED) == Z_OK && size + 64 < count)
                        {
                            request.Action = P::CompressedUploadChunk;
                            request.EssSize = count;
                            request.Count = static_cast<uint16_t>(size);
                        }
                        else
                        {
                            request.Action = P::UploadChunk;
                            request.Count = static_cast<uint16_t>(count);
                            std::copy_n(bytes, count, request.Data.begin());
                        }
                    }
                }
                const size_t charge = P::HeaderSize + request.Count + 1;
                if (uploading && s.UploadCredit < charge) break;
                if (!SendFlight(aTransport, request, s.SentOffset + count)) return;
                if (uploading) s.UploadCredit -= charge;
                if (request.Action == P::UploadCopy) s.ReusedBytes += count;
            }
            if (uploading && s.Offset == s.Bytes.size() && s.Flights.empty())
            {
                RequestCharacterSave request{};
                request.Revision = s.Revision;
                request.Action = P::Commit;
                Send(aTransport, request);
            }
            return;
        }
        if (s.Step == Phase::Staged && aTransport.IsOnline())
            Change(Phase::Ready, "Connected using the selected character slot.");
        if ((s.Step != Phase::NewCharacter && !(s.Step == Phase::Ready && aTransport.IsOnline())) ||
            now < s.NextPoll) return;
        // Once the named native save completes, only file I/O and the lobby connection
        // are needed to verify and upload it, even if Skyrim has returned to title.
        if (!s.CaptureReady && !aTransport.CanEnterWorld()) return;
        if (s.NativeEnabled && !s.CaptureReady) return;
        if (s.NativeEnabled && now >= s.NativeDeadline) { Fail("Completed native save files never became readable. Check the active profile save directory."); return; }
        s.NextPoll = now + std::chrono::seconds(1);
        PairStamp stamp{};
        if (!Snapshot(stamp, false) || stamp.Ess == s.Baseline.Ess ||
            (IsScriptExtenderLoaded() && stamp.Co == s.Baseline.Co))
        {
            s.StableSince = {};
            return;
        }
        if (stamp != s.Candidate || s.StableSince == Clock::time_point{})
        {
            s.Candidate = stamp;
            s.StableSince = now;
            return;
        }
        if (now - s.StableSince < std::chrono::seconds(5)) return;
        PairStamp captured{};
        if (!Snapshot(captured, true) || captured != stamp)
        {
            s.Bytes.clear();
            s.StableSince = {};
            return;
        }
        if (std::string_view(reinterpret_cast<const char*>(s.Bytes.data()), 13) != "TESV_SAVEGAME")
        {
            Fail("The named file is not a native Skyrim save."); return;
        }
        const auto digest = BlockDigest(0, s.Bytes.size());
        if (s.Revision && digest == s.Digest && captured.Ess.Size == s.Ess && captured.Co.Size == s.Co)
        {
            s.Baseline = captured;
            s.Bytes.clear();
            s.CaptureReady = false;
            s.NextAutoSave = now + kAutosaveInterval;
            Change(Phase::Ready, "The server already has this checkpoint.");
            return;
        }
        s.Captured = captured;
        s.Ess = captured.Ess.Size;
        s.Co = captured.Co.Size;
        ResetTransfer();
        s.ReusedBytes = 0;
        s.NeedsBegin = true;
        s.Digest = digest;
        if (!s.Revision) spdlog::info("Uploading the first character checkpoint ({} bytes)", s.Bytes.size());
        Change(Phase::Upload, "Uploading the completed native save...");
    }
    catch (...) { Fail("Checkpoint file access failed; reconnect to resume."); }
}
}
