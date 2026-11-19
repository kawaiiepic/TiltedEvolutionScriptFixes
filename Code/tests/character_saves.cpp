#include <catch2/catch.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include "../server/Campaign/CharacterSaveStore.h"
#include <cryptopp/sha.h>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>
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
        auto response = std::make_unique<P>();
        auto doRequest = [&](uint64_t session, bool world, const P& req) -> const P& {
            *response = {};
            store.Handle(session, world, req, *response);
            return *response;
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

TEST_CASE("Checkpoint revisions reuse matching blocks and verify the reconstructed save", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    const auto root = std::filesystem::temp_directory_path() /
        ("st-delta-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup
    {
        std::filesystem::path Path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); }
    } cleanup{root};
    Campaign::CharacterSaveStore store(root);
    auto req = std::make_unique<P>();
    auto reply = std::make_unique<P>();
    std::vector<uint8_t> bytes(P::ChunkSize + 3, 42);
    std::memcpy(bytes.data(), "TESV_SAVEGAME", 13);
    uint64_t revision = 0;
    auto send = [&](P::Operation action) {
        req->Action = action;
        req->Revision = revision;
        reply->Count = 0;
        store.Handle(1, revision != 0 && action != P::Select && action != P::DownloadIfChanged, *req, *reply);
        REQUIRE(reply->Status == P::Ok);
    };
    req->Key[0] = 1;
    send(P::Select);
    for (; revision < 2; ++revision)
    {
        bytes.back() = static_cast<uint8_t>(revision);
        req->Count = 0;
        req->EssSize = bytes.size();
        CryptoPP::SHA256().CalculateDigest(req->Digest.data(), bytes.data(), bytes.size());
        send(P::BeginUpload);
        req->Offset = 0;
        if (revision == 0)
        {
            std::copy_n(bytes.begin(), P::ChunkSize, req->Data.begin());
            req->Count = P::ChunkSize;
            send(P::UploadChunk);
        }
        else
            send(P::UploadCopy);
        REQUIRE(reply->Offset == P::ChunkSize);
        req->Offset = P::ChunkSize;
        req->Count = 3;
        std::copy(bytes.end() - 3, bytes.end(), req->Data.begin());
        send(P::UploadChunk);
        req->Count = 0;
        send(P::Commit);
        REQUIRE(reply->Revision == revision + 1);
    }
    store.Release(1);
    send(P::Select);
    req->Offset = 0;
    CryptoPP::SHA256().CalculateDigest(req->Digest.data(), bytes.data(), P::ChunkSize);
    send(P::DownloadIfChanged);
    REQUIRE(reply->Count == 0);
    REQUIRE_FALSE(store.CanEnter(1));
    req->Offset = P::ChunkSize;
    send(P::DownloadIfChanged);
    REQUIRE(reply->Count == 3);
    REQUIRE(std::equal(bytes.end() - 3, bytes.end(), reply->Data.begin()));
    REQUIRE(store.CanEnter(1));
}
