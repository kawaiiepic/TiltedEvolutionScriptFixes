#include <catch2/catch_test_macros.hpp>
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include "../server/Campaign/CharacterSaveStore.h"
#include <cryptopp/sha.h>
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
