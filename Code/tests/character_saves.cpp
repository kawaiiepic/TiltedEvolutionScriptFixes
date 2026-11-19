#include <catch2/catch.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include "../server/Campaign/CharacterSaveStore.h"
#include <cryptopp/sha.h>
#include <zlib.h>
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

TEST_CASE("Final checkpoint retry observes the committed revision before restarting an upload", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    const auto root = std::filesystem::temp_directory_path() /
        ("st-final-checkpoint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup
    {
        std::filesystem::path Path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); }
    } cleanup{root};

    Campaign::CharacterSaveStore store(root);
    auto request = std::make_unique<P>();
    auto reply = std::make_unique<P>();
    bool inWorld = false;
    auto send = [&](P::Operation action, uint64_t revision)
    {
        *reply = {};
        request->Action = action;
        request->Revision = revision;
        store.Handle(1, inWorld, *request, *reply);
        return reply->Status;
    };
    char bytes[] = "TESV_SAVEGAME checkpoint A";
    const size_t length = sizeof(bytes) - 1;
    auto begin = [&](uint64_t revision)
    {
        request->Count = 0;
        request->EssSize = length - 1;
        request->CoSaveSize = 1;
        CryptoPP::SHA256().CalculateDigest(request->Digest.data(), reinterpret_cast<const uint8_t*>(bytes), length);
        return send(P::BeginUpload, revision);
    };
    auto upload = [&](uint64_t revision)
    {
        request->Count = static_cast<uint16_t>(length);
        request->Offset = 0;
        std::memcpy(request->Data.data(), bytes, length);
        const auto status = send(P::UploadChunk, revision);
        request->Count = 0;
        return status;
    };

    request->Key[0] = 1;
    REQUIRE(send(P::Select, 0) == P::Ok);
    REQUIRE(begin(0) == P::Ok);
    REQUIRE(upload(0) == P::Ok);
    REQUIRE(send(P::Commit, 0) == P::Ok);
    REQUIRE(reply->Revision == 1);
    const auto originalDigest = reply->Digest;

    inWorld = true;
    bytes[length - 1] = 'B';
    REQUIRE(begin(1) == P::Ok);
    REQUIRE(send(P::Commit, 1) == P::Incomplete);
    REQUIRE(send(P::Abort, 1) == P::Ok);
    REQUIRE(reply->Revision == 1);
    REQUIRE(reply->Digest == originalDigest);

    REQUIRE(begin(1) == P::Ok);
    REQUIRE(upload(1) == P::Ok);
    REQUIRE(send(P::Commit, 1) == P::Ok); // Simulate a reply the client never processed.
    REQUIRE(send(P::Abort, 1) == P::Stale);
    REQUIRE(reply->Revision == 2);
    REQUIRE(reply->Digest == request->Digest);
    REQUIRE(reply->EssSize == length - 1);
    REQUIRE(reply->CoSaveSize == 1);
    const auto committedDigest = reply->Digest;

    bytes[length - 1] = 'C';
    REQUIRE(begin(2) == P::Ok);
    bytes[length - 1] = 'D';
    REQUIRE(upload(2) == P::Ok);
    REQUIRE(send(P::Commit, 2) == P::HashMismatch);
    REQUIRE(send(P::Abort, 2) == P::Ok);
    REQUIRE(reply->Revision == 2);
    REQUIRE(reply->Digest == committedDigest);

    store.Release(1);
    inWorld = false;
    REQUIRE(send(P::Select, 0) == P::Ok);
    REQUIRE(reply->Revision == 2);
    REQUIRE(reply->Digest == committedDigest);
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

TEST_CASE("Character picker follows committed server slots across reconnects and wipes", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    const auto root = std::filesystem::temp_directory_path() /
        ("st-roster-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup
    {
        std::filesystem::path Path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); }
    } cleanup{root};
    auto req = std::make_unique<P>();
    auto reply = std::make_unique<P>();
    Campaign::CharacterSaveStore store(root);
    auto send = [&](uint64_t session, P::Operation action) {
        req->Action = action;
        *reply = {};
        store.Handle(session, false, *req, *reply);
    };
    auto roster = [&](uint64_t session, uint8_t owner, const std::string& claims) {
        *req = {};
        req->Key[0] = owner;
        req->Count = static_cast<uint16_t>(claims.size());
        std::copy(claims.begin(), claims.end(), req->Data.begin());
        send(session, P::Roster);
        REQUIRE(reply->Status == P::Ok);
        return std::string(reinterpret_cast<const char*>(reply->Data.data()), reply->Count);
    };
    const std::string legacy = "01" + std::string(62, '0') + "Mia\n";
    REQUIRE(roster(1, 9, legacy).empty());
    bool migrate = false;
    SECTION("New slot") {}
    SECTION("Existing checkpoint from an older client") { migrate = true; }
    req->Key[0] = 1;
    req->Count = 3;
    std::memcpy(req->Data.data(), "Mia", 3);
    send(1, migrate ? P::Select : P::Create);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(roster(2, 9, "").empty());
    const std::string save = "TESV_SAVEGAME test";
    req->Count = 0;
    req->EssSize = save.size();
    CryptoPP::SHA256().CalculateDigest(req->Digest.data(), reinterpret_cast<const uint8_t*>(save.data()), save.size());
    send(1, P::BeginUpload);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(roster(2, 9, "").empty());
    req->Count = static_cast<uint16_t>(save.size());
    std::copy(save.begin(), save.end(), req->Data.begin());
    send(1, P::UploadChunk);
    REQUIRE(reply->Status == P::Ok);
    req->Count = static_cast<uint16_t>(P::PortraitBytes);
    std::fill_n(req->Data.begin(), P::PortraitBytes, uint8_t{42});
    send(1, P::Commit);
    REQUIRE(reply->Status == P::Ok);
    store.Release(1);
    const auto listed = roster(2, 9, migrate ? legacy : "");
    REQUIRE(listed.size() == 68);
    REQUIRE(listed.substr(64) == "Mia\n");
    store.Release(2);
    REQUIRE(roster(3, 9, "") == listed);
    REQUIRE(roster(4, 8, legacy).empty());
    std::array<uint8_t, 32> id{};
    for (size_t i = 0; i < id.size(); ++i)
        id[i] = static_cast<uint8_t>(std::stoul(listed.substr(2 * i, 2), nullptr, 16));
    *req = {};
    req->Key = id;
    send(4, P::Preview);
    REQUIRE(reply->Status == P::Missing);
    send(3, P::Preview);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(reply->Key == id);
    REQUIRE(reply->Count == P::PortraitBytes + 2);
    REQUIRE(reply->Data[0] == 0);
    REQUIRE(reply->Data[1] == 0);
    REQUIRE(reply->Data[2] == 42);
    REQUIRE(reply->Data[reply->Count - 1] == 42);
    send(4, P::SelectOwned);
    REQUIRE(reply->Status == P::Missing);
    send(3, P::SelectOwned);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(reply->Revision == 1);
    store.Release(3);
    std::filesystem::remove_all(root);
    REQUIRE(roster(5, 9, legacy).empty());
}

TEST_CASE("Compressed checkpoint blocks are bounded and verified", "[campaign.saves]")
{
    using P = CharacterSavePayload;
    const auto root = std::filesystem::temp_directory_path() /
        ("st-checkpoint-compressed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path Path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(Path, ec); } } cleanup{root};
    Campaign::CharacterSaveStore store(root);
    auto request = std::make_unique<P>();
    auto reply = std::make_unique<P>();
    auto send = [&](uint8_t action) {
        request->Action = action;
        *reply = {};
        store.Handle(1, false, *request, *reply);
    };
    request->Key[0] = 17;
    send(P::Select);
    REQUIRE(reply->Status == P::Ok);
    std::vector<uint8_t> bytes(P::ChunkSize, 42);
    std::memcpy(bytes.data(), "TESV_SAVEGAME", 13);
    request->EssSize = bytes.size();
    CryptoPP::SHA256().CalculateDigest(request->Digest.data(), bytes.data(), bytes.size());
    send(P::BeginUpload);
    REQUIRE(reply->Status == P::Ok);
    uLongf length = request->Data.size();
    REQUIRE(compress2(request->Data.data(), &length, bytes.data(), static_cast<uLong>(bytes.size()), Z_BEST_SPEED) == Z_OK);
    request->Count = static_cast<uint16_t>(length);
    request->EssSize = bytes.size() + 1;
    send(P::CompressedUploadChunk);
    REQUIRE(reply->Status == P::Invalid);
    request->EssSize = bytes.size();
    send(P::CompressedUploadChunk);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(reply->Offset == bytes.size());
    request->Count = 0;
    send(P::Commit);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(reply->Revision == 1);
    store.Release(1);
    send(P::Select);
    REQUIRE(reply->Revision == 1);
    request->Revision = 1;
    send(P::DownloadChunk);
    REQUIRE(reply->Status == P::Ok);
    REQUIRE(reply->Count == bytes.size());
    REQUIRE(std::equal(bytes.begin(), bytes.end(), reply->Data.begin()));
}
