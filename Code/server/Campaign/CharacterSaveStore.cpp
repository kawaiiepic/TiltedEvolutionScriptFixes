#include "CharacterSaveStore.h"
#include "DurableAppend.h"

#include <cryptopp/sha.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <system_error>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Campaign
{
namespace
{
using P = CharacterSavePayload;
constexpr std::array<uint8_t, 8> kMagic{'S','T','S','A','V','E','0','1'};

void Put64(uint8_t* aData, uint64_t aValue)
{
    for (unsigned i = 0; i < 8; ++i)
        aData[i] = static_cast<uint8_t>(aValue >> (8 * i));
}

uint64_t Get64(const uint8_t* aData)
{
    uint64_t value{};
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(aData[i]) << (8 * i);
    return value;
}

bool ValidSize(uint64_t aEss, uint64_t aCoSave)
{
    return aEss >= 13 && aEss <= P::MaxSize && aCoSave <= P::MaxSize - aEss;
}

std::string Slot(const std::array<uint8_t, 32>& acKey)
{
    std::array<uint8_t, 32> hash{};
    CryptoPP::SHA256().CalculateDigest(hash.data(), acKey.data(), acKey.size());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : hash)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool SyncDirectory(const std::filesystem::path& acPath)
{
#ifdef _WIN32
    (void)acPath; // Final publication uses MoveFileExW with WRITE_THROUGH.
    return true;
#else
    const int directory = open(acPath.empty() ? "." : acPath.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) return false;
    const bool ok = fsync(directory) == 0;
    close(directory);
    return ok;
#endif
}

bool Publish(const std::filesystem::path& acPending, const std::filesystem::path& acFinal)
{
    std::error_code ec;
    if (std::filesystem::exists(acFinal, ec) || ec)
        return false;
    if (!AppendDurably(acPending, "", false))
        return false;
#ifdef _WIN32
    return MoveFileExW(acPending.c_str(), acFinal.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(acPending, acFinal, ec);
    if (ec) return false;
    return SyncDirectory(acFinal.parent_path());
#endif
}
}

CharacterSaveStore::CharacterSaveStore(std::filesystem::path aRoot) : m_root(std::move(aRoot)) {}
CharacterSaveStore::~CharacterSaveStore()
{
    while (!m_leases.empty()) Release(m_leases.begin()->first);
}

std::filesystem::path CharacterSaveStore::File(const Lease& acLease, uint64_t aRevision)
{
    return acLease.Directory / (std::to_string(aRevision) + ".checkpoint");
}

bool CharacterSaveStore::ReadHeader(const std::filesystem::path& acPath, Revision& aRevision)
{
    std::array<uint8_t, HeaderSize> header{};
    std::ifstream input(acPath, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        !std::equal(kMagic.begin(), kMagic.end(), header.begin()))
        return false;
    aRevision.Ess = Get64(header.data() + 8);
    aRevision.CoSave = Get64(header.data() + 16);
    std::copy_n(header.begin() + 24, 32, aRevision.Digest.begin());
    std::error_code ec;
    const auto size = std::filesystem::file_size(acPath, ec);
    return !ec && ValidSize(aRevision.Ess, aRevision.CoSave) &&
        size == HeaderSize + aRevision.Ess + aRevision.CoSave;
}

bool CharacterSaveStore::Discover(Lease& aLease)
{
    std::error_code ec;
    if (!std::filesystem::exists(aLease.Directory, ec))
        return !ec;
    for (std::filesystem::directory_iterator it(aLease.Directory, ec), end; !ec && it != end; it.increment(ec))
    {
        const auto& path = it->path();
        if (path.extension() != ".checkpoint")
            continue;
        const std::string name = path.stem().string();
        uint64_t revision{};
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), revision);
        if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || !revision || std::to_string(revision) != name)
            return false;
        aLease.Latest.Number = std::max(revision, aLease.Latest.Number);
    }
    if (ec) return false;
    if (!aLease.Latest.Number) return true;
    // Never silently fall back to an older revision if the latest is damaged.
    return ReadHeader(File(aLease, aLease.Latest.Number), aLease.Latest);
}

void CharacterSaveStore::Describe(const Revision& acRevision, CharacterSavePayload& aReply) noexcept
{
    aReply.Revision = acRevision.Number;
    aReply.EssSize = acRevision.Ess;
    aReply.CoSaveSize = acRevision.CoSave;
    aReply.Digest = acRevision.Digest;
}

void CharacterSaveStore::Discard(Lease& aLease) noexcept
{
    if (aLease.Output.is_open()) aLease.Output.close();
    if (aLease.Input.is_open()) aLease.Input.close();
    aLease.Output.clear();
    aLease.Input.clear();
    std::error_code ec;
    std::filesystem::remove(aLease.Directory / "upload.pending", ec);
    aLease.Uploading = false;
    aLease.Written = 0;
    aLease.Upload = {};
}

void CharacterSaveStore::Release(uint64_t aSession) noexcept
{
    const auto it = m_leases.find(aSession);
    if (it == m_leases.end()) return;
    Discard(it->second);
    std::error_code ec;
    // Only removes an empty directory. Published revisions are never removed here.
    if (!it->second.Latest.Number)
        std::filesystem::remove(it->second.Directory, ec);
    m_leases.erase(it);
}

bool CharacterSaveStore::CanEnter(uint64_t aSession) const noexcept
{
    const auto it = m_leases.find(aSession);
    return it != m_leases.end() && it->second.Ready && !it->second.Uploading && !it->second.Broken && it->second.Latest.Number;
}

void CharacterSaveStore::Handle(uint64_t aSession, bool aInWorld, const CharacterSavePayload& acRequest, CharacterSavePayload& aReply)
{
    aReply.Action = acRequest.Action;
    aReply.Status = P::Invalid;
    if (acRequest.Status || acRequest.Action > P::ReuseChunk || acRequest.Count > P::ChunkSize)
        return;

    if (acRequest.Action == P::Select)
    {
        if (aInWorld || m_leases.contains(aSession) ||
            std::all_of(acRequest.Key.begin(), acRequest.Key.end(), [](uint8_t b) { return b == 0; }))
            return;
        Lease lease;
        lease.Directory = m_root / Slot(acRequest.Key);
        for (const auto& [id, other] : m_leases)
        {
            if (other.Directory == lease.Directory)
            {
                aReply.Status = P::Busy;
                return;
            }
        }
        if (!Discover(lease))
        {
            aReply.Status = P::IoError;
            return;
        }
        Discard(lease); // remove an incomplete upload after a process crash
        Describe(lease.Latest, aReply);
        m_leases.emplace(aSession, std::move(lease));
        aReply.Status = P::Ok;
        return;
    }

    const auto it = m_leases.find(aSession);
    if (it == m_leases.end()) return;
    auto& lease = it->second;
    if (lease.Broken)
    {
        aReply.Status = P::IoError;
        return;
    }
    Describe(lease.Latest, aReply);
    if (acRequest.Revision != lease.Latest.Number)
    {
        aReply.Status = P::Stale;
        return;
    }
    const auto pending = lease.Directory / "upload.pending";
    switch (acRequest.Action)
    {
    case P::BeginUpload:
    {
        // Returning characters must download the canonical checkpoint before
        // joining. Never accept a replacement local save in their lobby.
        if (lease.Uploading || (lease.Latest.Number && (!aInWorld || !lease.Ready)) ||
            !ValidSize(acRequest.EssSize, acRequest.CoSaveSize) ||
            lease.Latest.Number == std::numeric_limits<uint64_t>::max())
            return;
        std::error_code ec;
        std::filesystem::create_directories(m_root, ec);
        if (ec || !SyncDirectory(m_root.parent_path())) { aReply.Status = P::IoError; return; }
        if (!std::filesystem::exists(lease.Directory, ec))
        {
            if (ec) { aReply.Status = P::IoError; return; }
            size_t count{};
            for (std::filesystem::directory_iterator pos(m_root, ec), end; !ec && pos != end; pos.increment(ec))
                ++count;
            if (ec) { aReply.Status = P::IoError; return; }
            if (count >= MaxSlots) { aReply.Status = P::Quota; return; }
            std::filesystem::create_directory(lease.Directory, ec);
            if (ec) { aReply.Status = P::IoError; return; }
        }

        if (!SyncDirectory(m_root)) { aReply.Status = P::IoError; return; }
        lease.Upload = {lease.Latest.Number + 1, acRequest.EssSize, acRequest.CoSaveSize, acRequest.Digest};
        std::array<uint8_t, HeaderSize> header{};
        std::copy(kMagic.begin(), kMagic.end(), header.begin());
        Put64(header.data() + 8, lease.Upload.Ess);
        Put64(header.data() + 16, lease.Upload.CoSave);
        std::copy(lease.Upload.Digest.begin(), lease.Upload.Digest.end(), header.begin() + 24);
        lease.Output.open(pending, std::ios::binary | std::ios::trunc);
        lease.Output.write(reinterpret_cast<const char*>(header.data()), header.size());
        if (!lease.Output) { Discard(lease); aReply.Status = P::IoError; return; }
        lease.Written = 0;
        lease.Uploading = true;
        aReply.Status = P::Ok;
        return;
    }
    case P::UploadChunk:
    case P::CompressedUploadChunk:
    case P::ReuseChunk:
    {
        // Compressed chunks declare their bounded decoded length in EssSize.
        // Reuse is restricted to the same offset of this lease's latest revision.
        const uint64_t length = acRequest.Action == P::CompressedUploadChunk ? acRequest.EssSize : acRequest.Count;
        if (!lease.Uploading || !acRequest.Count || !length || length > P::ChunkSize ||
            acRequest.Offset != lease.Written || length > lease.Upload.Ess + lease.Upload.CoSave - lease.Written)
            return;
        std::array<uint8_t, P::ChunkSize> decoded{};
        const uint8_t* bytes = acRequest.Data.data();
        if (acRequest.Action == P::CompressedUploadChunk)
        {
            uLongf actual = static_cast<uLongf>(length);
            if (uncompress(decoded.data(), &actual, bytes, acRequest.Count) != Z_OK || actual != length)
                return;
            bytes = decoded.data();
        }
        else if (acRequest.Action == P::ReuseChunk)
        {
            const uint64_t previousSize = lease.Latest.Ess + lease.Latest.CoSave;
            if (!lease.Latest.Number || lease.Written > previousSize || length > previousSize - lease.Written)
                return;
            if (!lease.Input.is_open())
                lease.Input.open(File(lease, lease.Latest.Number), std::ios::binary);
            lease.Input.seekg(static_cast<std::streamoff>(HeaderSize + lease.Written));
            if (!lease.Input.read(reinterpret_cast<char*>(decoded.data()), static_cast<std::streamsize>(length)))
            {
                Discard(lease); aReply.Status = P::IoError; return;
            }
            bytes = decoded.data();
        }
        lease.Output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(length));
        if (!lease.Output) { Discard(lease); aReply.Status = P::IoError; return; }
        lease.Written += length;
        aReply.Offset = lease.Written;
        aReply.Status = P::Ok;
        return;
    }
    case P::Commit:
    {
        if (!lease.Uploading || lease.Written != lease.Upload.Ess + lease.Upload.CoSave)
        {
            aReply.Status = P::Incomplete;
            return;
        }
        lease.Output.close();
        if (!lease.Output) { Discard(lease); aReply.Status = P::IoError; return; }
        if (lease.Input.is_open()) lease.Input.close();
        Revision header{};
        if (!ReadHeader(pending, header))
        {
            Discard(lease);
            aReply.Status = P::IoError;
            return;
        }
        std::ifstream input(pending, std::ios::binary);
        input.seekg(HeaderSize);
        std::array<char, 13> magic{};
        input.read(magic.data(), magic.size());
        if (!input || std::string_view(magic.data(), magic.size()) != "TESV_SAVEGAME")
        {
            input.close(); Discard(lease); return;
        }
        input.seekg(HeaderSize);
        CryptoPP::SHA256 hash;
        std::array<uint8_t, 65536> data{};
        uint64_t remaining = lease.Written;
        while (remaining)
        {
            const auto length = static_cast<size_t>(std::min<uint64_t>(remaining, data.size()));
            if (!input.read(reinterpret_cast<char*>(data.data()), length))
            {
                input.close(); Discard(lease); aReply.Status = P::IoError; return;
            }
            hash.Update(data.data(), length);
            remaining -= length;
        }
        input.close();
        std::array<uint8_t, 32> digest{};
        hash.Final(digest.data());
        if (digest != lease.Upload.Digest)
        {
            Discard(lease); aReply.Status = P::HashMismatch; return;
        }
        if (!Publish(pending, File(lease, lease.Upload.Number)))
        {
            // A failed directory flush might have published the file. Fail
            // closed until reconnect/recovery instead of reusing its revision.
            lease.Broken = true;
            Discard(lease);
            aReply.Status = P::IoError;
            return;
        }
        lease.Latest = lease.Upload;
        lease.Ready = true;
        Discard(lease);
        Describe(lease.Latest, aReply);
        aReply.Status = P::Ok;
        // Retain the latest and previous revision. Cleanup is best effort
        // and never precedes commit of the new checkpoint.
        if (lease.Latest.Number > 2)
        {
            std::error_code ec;
            for (std::filesystem::directory_iterator pos(lease.Directory, ec), end; !ec && pos != end; pos.increment(ec))
            {
                if (pos->path().extension() != ".checkpoint") continue;
                const auto name = pos->path().stem().string();
                uint64_t number{};
                const auto parsed = std::from_chars(name.data(), name.data() + name.size(), number);
                if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() &&
                    number && number < lease.Latest.Number - 1)
                {
                    std::error_code ignored;
                    std::filesystem::remove(pos->path(), ignored);
                }
            }
        }
        return;
    }
    case P::DownloadChunk:
    {
        if (aInWorld || !lease.Latest.Number || lease.Uploading)
        {
            aReply.Status = P::Missing;
            return;
        }
        const uint64_t total = lease.Latest.Ess + lease.Latest.CoSave;
        if (acRequest.Offset != lease.Downloaded || acRequest.Offset >= total)
            return;
        aReply.Count = static_cast<uint16_t>(std::min<uint64_t>(P::ChunkSize, total - acRequest.Offset));
        if (!lease.Input.is_open())
            lease.Input.open(File(lease, lease.Latest.Number), std::ios::binary);
        lease.Input.seekg(static_cast<std::streamoff>(HeaderSize + acRequest.Offset));
        if (!lease.Input.read(reinterpret_cast<char*>(aReply.Data.data()), aReply.Count))
        {
            aReply.Count = 0; aReply.Status = P::IoError; return;
        }
        aReply.Offset = acRequest.Offset;
        lease.Downloaded += aReply.Count;
        lease.Ready = lease.Downloaded == total;
        if (lease.Ready) lease.Input.close();
        aReply.Status = P::Ok;
        return;
    }
    case P::Abort:
        Discard(lease);
        aReply.Status = P::Ok;
        return;
    default: return;
    }
}
}
