#include <TiltedOnlinePCH.h>
#include "CharacterCheckpoints.h"

#include <Messages/CharacterSave.h>
#include <Services/TransportService.h>
#include <Services/OverlayService.h>
#include <ScriptExtender.h>
#include <World.h>
#include <OverlayApp.hpp>
#include <cryptopp/sha.h>
#include <cryptopp/osrng.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>
#include <vector>

namespace CharacterCheckpoints
{
namespace
{
using P = CharacterSavePayload;
using Clock = std::chrono::steady_clock;
enum class Phase { Disabled, Choose, Selecting, Download, NewCharacter, Ready, Upload, Failed };
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
struct State
{
    Phase Step{Phase::Disabled};
    bool Required{}, Pending{}, NeedsBegin{};
    uint8_t Expected{};
    uint64_t Revision{}, Ess{}, Co{}, Offset{};
    uint16_t LastCount{};
    std::array<uint8_t, 32> Digest{};
    std::vector<uint8_t> Bytes;
    std::filesystem::path Directory;
    std::string Name, Detail;
    PairStamp Baseline{}, Candidate{}, Captured{};
    Clock::time_point StableSince{}, NextPoll{}, Deadline{};
};
State s;
constexpr char kHex[] = "0123456789abcdef";

const char* PhaseName() noexcept
{
    switch (s.Step)
    {
    case Phase::Disabled: return "disabled";
    case Phase::Choose: return "choose";
    case Phase::Selecting: return "selecting";
    case Phase::Download: return "downloading";
    case Phase::NewCharacter: return "new";
    case Phase::Ready: return "ready";
    case Phase::Upload: return "uploading";
    default: return "failed";
    }
}
void Change(Phase aPhase, const char* apDetail)
{
    s.Step = aPhase;
    s.Detail = apDetail;
    PublishState();
}
void Fail(const char* apDetail)
{
    s.Pending = false;
    s.Bytes.clear();
    Change(Phase::Failed, apDetail);
    spdlog::warn("Character checkpoint: {}", apDetail);
}

std::filesystem::path SaveDirectory()
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
    return std::filesystem::path(documents) / L"My Games" / L"Skyrim Special Edition" / L"Saves";
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
    s.Bytes.clear();
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
    overlay->ExecuteAsync("characterCheckpoint", args);
}
void Begin(bool aRequired) noexcept
{
    s = {};
    s.Required = aRequired;
    Change(aRequired ? Phase::Choose : Phase::Disabled, aRequired ? "Choose a character slot." : "");
}
void Reset() noexcept
{
    s = {};
    PublishState();
}
bool CanEnter() noexcept
{
    return !s.Required || (s.Step == Phase::Ready && !s.Pending && s.Revision);
}

void Select(TransportService& aTransport, const std::string& acKey) noexcept
{
    if (!aTransport.IsInLobby() || s.Step != Phase::Choose || acKey.size() != 64) return;
    try
    {
        RequestCharacterSave request{};
        request.Action = P::Select;
        for (size_t i = 0; i < request.Key.size(); ++i)
        {
            const auto hi = std::string_view(kHex, 16).find(acKey[2 * i]);
            const auto lo = std::string_view(kHex, 16).find(acKey[2 * i + 1]);
            if (hi == std::string_view::npos || lo == std::string_view::npos) return;
            request.Key[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        if (!ChoosePath()) { Change(Phase::Choose, "Save directory unavailable; configure ST_SAVE_DIRECTORY."); return; }
        Change(Phase::Selecting, "Checking server character...");
        Send(aTransport, request);
    }
    catch (...) { Fail("Could not prepare a character save directory."); }
}

void Receive(const NotifyCharacterSave& acReply) noexcept
{
    if (!s.Required || !s.Pending || acReply.Action != s.Expected) return;
    s.Pending = false;
    try
    {
        if (acReply.Status != P::Ok)
        {
            if (s.Step == Phase::Selecting)
            {
                Change(Phase::Choose, acReply.Status == P::Busy ? "Character is already in use." : "Server could not select the character.");
                return;
            }
            Fail("Server rejected checkpoint transfer; reconnect to resume.");
            return;
        }
        switch (acReply.Action)
        {
        case P::Select:
            s.Revision = acReply.Revision;
            if (!s.Revision)
            {
                Change(Phase::NewCharacter, "Create your character in Skyrim, then make the named checkpoint save. It uploads automatically.");
                return;
            }
            if (acReply.EssSize < 13 || acReply.EssSize > P::MaxSize ||
                acReply.CoSaveSize > P::MaxSize - acReply.EssSize) { Fail("Invalid save size."); return; }
            s.Ess = acReply.EssSize;
            s.Co = acReply.CoSaveSize;
            s.Digest = acReply.Digest;
            s.Offset = 0;
            s.Bytes.resize(static_cast<size_t>(s.Ess + s.Co));
            Change(Phase::Download, "Downloading the server's latest checkpoint...");
            return;
        case P::DownloadChunk:
            if (s.Step != Phase::Download || acReply.Revision != s.Revision ||
                acReply.EssSize != s.Ess || acReply.CoSaveSize != s.Co || acReply.Digest != s.Digest ||
                acReply.Offset != s.Offset || !acReply.Count || acReply.Count > P::ChunkSize ||
                acReply.Count > s.Bytes.size() - s.Offset) { Fail("Invalid download response."); return; }
            std::copy_n(acReply.Data.begin(), acReply.Count, s.Bytes.begin() + static_cast<size_t>(s.Offset));
            s.Offset += acReply.Count;
            if (s.Offset == s.Bytes.size())
            {
                if (!StageDownload()) { Fail("Checkpoint failed integrity verification or could not be staged."); return; }
                Change(Phase::Ready, "Downloaded. Load the named save in Skyrim, then select Enter world.");
            }
            return;
        case P::BeginUpload:
            if (s.Step != Phase::Upload || acReply.Revision != s.Revision) { Fail("Invalid upload response."); return; }
            s.NeedsBegin = false;
            return;
        case P::UploadChunk:
            if (s.Step != Phase::Upload || acReply.Revision != s.Revision ||
                acReply.Offset != s.Offset + s.LastCount) { Fail("Invalid upload offset."); return; }
            s.Offset = acReply.Offset;
            return;
        case P::Commit:
            if (s.Step != Phase::Upload || acReply.Revision != s.Revision + 1 || acReply.Digest != s.Digest ||
                acReply.EssSize != s.Ess || acReply.CoSaveSize != s.Co) { Fail("Invalid commit response."); return; }
            s.Revision = acReply.Revision;
            s.Baseline = s.Captured;
            s.Bytes.clear();
            Change(Phase::Ready, "Checkpoint saved on the server.");
            return;
        default: Fail("Unexpected checkpoint response."); return;
        }
    }
    catch (...) { Fail("Unable to complete checkpoint transfer."); }
}

void Tick(TransportService& aTransport) noexcept
{
    if (!s.Required || (!aTransport.IsInLobby() && !aTransport.IsOnline())) return;
    try
    {
        const auto now = Clock::now();
        if (s.Pending)
        {
            if (now >= s.Deadline) Fail("Checkpoint transfer timed out; reconnect to resume.");
            return;
        }
        RequestCharacterSave request{};
        request.Revision = s.Revision;
        if (s.Step == Phase::Download)
        {
            request.Action = P::DownloadChunk;
            request.Offset = s.Offset;
            Send(aTransport, request);
            return;
        }
        if (s.Step == Phase::Upload)
        {
            if (s.NeedsBegin)
            {
                request.Action = P::BeginUpload;
                request.EssSize = s.Ess;
                request.CoSaveSize = s.Co;
                request.Digest = s.Digest;
            }
            else if (s.Offset < s.Bytes.size())
            {
                request.Action = P::UploadChunk;
                request.Offset = s.Offset;
                request.Count = static_cast<uint16_t>(std::min<uint64_t>(P::ChunkSize, s.Bytes.size() - s.Offset));
                s.LastCount = request.Count;
                std::copy_n(s.Bytes.begin() + static_cast<size_t>(s.Offset), request.Count, request.Data.begin());
            }
            else request.Action = P::Commit;
            Send(aTransport, request);
            return;
        }
        if ((s.Step != Phase::NewCharacter && !(s.Step == Phase::Ready && aTransport.IsOnline())) ||
            now < s.NextPoll || !aTransport.CanEnterWorld()) return;
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
        s.Captured = captured;
        s.Ess = captured.Ess.Size;
        s.Co = captured.Co.Size;
        s.Offset = 0;
        s.NeedsBegin = true;
        CryptoPP::SHA256().CalculateDigest(s.Digest.data(), s.Bytes.data(), s.Bytes.size());
        Change(Phase::Upload, "Uploading the completed native save...");
    }
    catch (...) { Fail("Checkpoint file access failed; reconnect to resume."); }
}
}
