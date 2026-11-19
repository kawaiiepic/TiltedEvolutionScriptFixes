#pragma once

#include <Messages/CharacterSave.h>
#include <filesystem>
#include <map>
#include <string>

namespace Campaign
{
// A character capability identifies one private save slot. Connection IDs
// are only used for process-local exclusive leases, never persistent identity.
class CharacterSaveStore
{
public:
    explicit CharacterSaveStore(std::filesystem::path aRoot);
    ~CharacterSaveStore();
    void Handle(uint64_t aSession, bool aInWorld, const CharacterSavePayload& acRequest, CharacterSavePayload& aReply);
    bool CanEnter(uint64_t aSession) const noexcept;
    void Release(uint64_t aSession) noexcept;

private:
    struct Revision
    {
        uint64_t Number{}, Ess{}, CoSave{};
        std::array<uint8_t, 32> Digest{};
    };
    struct Lease
    {
        std::filesystem::path Directory;
        Revision Latest, Upload;
        std::string Owner, Label;
        uint64_t Written{}, Downloaded{};
        bool Uploading{}, Ready{}, Broken{};
    };
    static constexpr size_t MaxSlots = 128;
    static constexpr size_t HeaderSize = 56;
    static std::filesystem::path File(const Lease& acLease, uint64_t aRevision);
    static bool ReadHeader(const std::filesystem::path& acPath, Revision& aRevision);
    static bool Discover(Lease& aLease);
    static void Describe(const Revision& acRevision, CharacterSavePayload& aReply) noexcept;
    static void Discard(Lease& aLease) noexcept;
    void ListCharacters(uint64_t aSession, const CharacterSavePayload& acRequest, CharacterSavePayload& aReply);
    std::map<uint64_t, std::string> m_owners;
    std::filesystem::path m_root;
    std::map<uint64_t, Lease> m_leases;
};
}
