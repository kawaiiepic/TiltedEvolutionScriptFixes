#include <catch2/catch.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include "../server/Campaign/CharacterSaveStore.h"
#include <cryptopp/sha.h>
#include <zlib.h>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cstring>
#include <filesystem>

TEST_CASE("Character checkpoints enforce leases, revisions and complete transfers", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    const auto root = std::filesystem::temp_directory_path() /
        ("st-checkpoint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup
    {
        std::filesystem::path Path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); }
    } cleanup{root};
    P select{};
    select.Action = P::Select;
    select.Key[0] = 1;
    P data{};
    const char bytes[] = "TESV_SAVEGAME test payload";
    const auto length = sizeof(bytes) - 1;
    {
        Campaign::CharacterSaveStore store(root);
        auto doRequest = [&](uint64_t session, bool world, const P& req) {
            P reply{};
            store.Handle(session, world, req, reply);
            return reply;
        };
        REQUIRE(doRequest(1, false, select).Status == P::Ok);
        REQUIRE(doRequest(2, false, select).Status == P::Busy);
        REQUIRE_FALSE(store.CanEnter(1));
        P begin{};
        begin.Action = P::BeginUpload;
        begin.EssSize = length;
        CryptoPP::SHA256().CalculateDigest(begin.Digest.data(), reinterpret_cast<const uint8_t*>(bytes), length);
        REQUIRE(doRequest(1, false, begin).Status == P::Ok);
        P commit{};
        commit.Action = P::Commit;
        REQUIRE(doRequest(1, false, commit).Status == P::Incomplete);
        data.Action = P::UploadChunk;
        data.Count = static_cast<uint16_t>(length);
        std::memcpy(data.Data.data(), bytes, length);
        data.Offset = 1;
        REQUIRE(doRequest(1, false, data).Status == P::Invalid);
        data.Offset = 0;
        REQUIRE(doRequest(1, false, data).Status == P::Ok);
        REQUIRE(doRequest(1, false, commit).Revision == 1);
        REQUIRE(store.CanEnter(1));
        REQUIRE(doRequest(1, true, begin).Status == P::Stale);
        store.Release(1);
    }
    {
        Campaign::CharacterSaveStore store(root);
        P reply{};
        store.Handle(3, false, select, reply);
        REQUIRE(reply.Status == P::Ok);
        REQUIRE(reply.Revision == 1);
        REQUIRE_FALSE(store.CanEnter(3));
        P download{};
        download.Action = P::DownloadChunk;
        download.Revision = 1;
        reply = {};
        store.Handle(3, false, download, reply);
        REQUIRE(reply.Status == P::Ok);
        REQUIRE(reply.Count == length);
        REQUIRE(std::memcmp(reply.Data.data(), bytes, length) == 0);
        REQUIRE(store.CanEnter(3));
    }
}

TEST_CASE("Checkpoint packets use compact headers and bounded data", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    RequestCharacterSave upload{};
    upload.Action = P::CompressedUploadChunk;
    upload.Count = 21;
    upload.EssSize = P::ChunkSize;
    upload.Data[0] = 42;
    TiltedPhoques::Buffer buffer(P::WireSize);
    TiltedPhoques::Buffer::Writer writer(&buffer);
    upload.Serialize(writer);
    REQUIRE(writer.Size() == P::HeaderWireSize + upload.Count);
    REQUIRE(P::ValidWireSize(buffer.GetWriteData(), static_cast<uint32_t>(writer.Size()), true));
    REQUIRE_FALSE(P::ValidWireSize(buffer.GetWriteData(), static_cast<uint32_t>(writer.Size() - 1), true));
    REQUIRE_FALSE(P::ValidWireSize(buffer.GetWriteData(), static_cast<uint32_t>(writer.Size()), false));
    NotifyCharacterSave ack{};
    ack.Action = upload.Action;
    ack.Offset = P::ChunkSize;
    TiltedPhoques::Buffer ackBuffer(P::WireSize);
    TiltedPhoques::Buffer::Writer ackWriter(&ackBuffer);
    ack.Serialize(ackWriter);
    REQUIRE(ackWriter.Size() == P::HeaderWireSize);
    REQUIRE(P::ValidWireSize(ackBuffer.GetWriteData(), static_cast<uint32_t>(ackWriter.Size()), false));
}

TEST_CASE("Compressed and reused blocks preserve the complete canonical save", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    const auto root = std::filesystem::temp_directory_path() /
        ("st-checkpoint-blocks-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path Path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); } } cleanup{root};
    std::vector<uint8_t> bytes(P::ChunkSize * 2 + 17, 42);
    std::memcpy(bytes.data(), "TESV_SAVEGAME", 13);
    Campaign::CharacterSaveStore store(root);
    auto send = [&](const P& req, bool world = false) {
        P reply{};
        store.Handle(1, world, req, reply);
        return reply;
    };
    P request{};
    request.Action = P::Select; request.Key[0] = 5;
    REQUIRE(send(request).Status == P::Ok);
    auto begin = [&](uint64_t revision) {
        P req{};
        req.Action = P::BeginUpload; req.Revision = revision; req.EssSize = bytes.size();
        CryptoPP::SHA256().CalculateDigest(req.Digest.data(), bytes.data(), bytes.size());
        return send(req, revision != 0);
    };
    REQUIRE(begin(0).Status == P::Ok);
    for (size_t offset = 0; offset < bytes.size(); offset += P::ChunkSize)
    {
        P req{};
        req.Action = P::CompressedUploadChunk; req.Offset = offset;
        req.EssSize = std::min(P::ChunkSize, bytes.size() - offset);
        uLongf length = req.Data.size();
        REQUIRE(compress2(req.Data.data(), &length, bytes.data() + offset, static_cast<uLong>(req.EssSize), Z_BEST_SPEED) == Z_OK);
        req.Count = static_cast<uint16_t>(length);
        const auto reply = send(req);
        REQUIRE(reply.Status == P::Ok);
        REQUIRE(reply.Offset == offset + req.EssSize);
    }
    request = {}; request.Action = P::Commit;
    REQUIRE(send(request).Revision == 1);
    bytes[P::ChunkSize + 1] = 99;
    REQUIRE(begin(1).Status == P::Ok);
    for (size_t offset = 0; offset < bytes.size(); offset += P::ChunkSize)
    {
        P req{};
        req.Revision = 1; req.Offset = offset;
        req.Count = static_cast<uint16_t>(std::min(P::ChunkSize, bytes.size() - offset));
        req.Action = offset == P::ChunkSize ? P::UploadChunk : P::ReuseChunk;
        if (req.Action == P::UploadChunk) std::memcpy(req.Data.data(), bytes.data() + offset, req.Count);
        const auto reply = send(req, true);
        REQUIRE(reply.Status == P::Ok);
        REQUIRE(reply.Offset == offset + req.Count);
    }
    request = {}; request.Revision = 1; request.Action = P::Commit;
    REQUIRE(send(request, true).Revision == 2);
    store.Release(1);
    request = {}; request.Action = P::Select; request.Key[0] = 5;
    REQUIRE(send(request).Revision == 2);
    for (size_t offset = 0; offset < bytes.size(); offset += P::ChunkSize)
    {
        request = {}; request.Revision = 2; request.Action = P::DownloadChunk; request.Offset = offset;
        const auto reply = send(request);
        REQUIRE(reply.Status == P::Ok);
        REQUIRE(reply.Count == std::min(P::ChunkSize, bytes.size() - offset));
        REQUIRE(std::memcmp(reply.Data.data(), bytes.data() + offset, reply.Count) == 0);
    }
    REQUIRE(store.CanEnter(1));
}
