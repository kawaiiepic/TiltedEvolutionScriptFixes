#include <Messages/NotifyTravelInvitation.h>
#include <TiltedCore/Serialization.hpp>

void NotifyTravelInvitation::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    Serialization::WriteVarInt(aWriter, RemainingMs);
    Serialization::WriteVarInt(aWriter, ParticipantCount);
    aWriter.WriteBits(CanCancel, 1);
    aWriter.WriteBits(Accepted, 1);
    CellId.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Serialization::WriteString(aWriter, LeaderName);
}

void NotifyTravelInvitation::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Id = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    RemainingMs = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    ParticipantCount = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    uint64_t flag{};
    aReader.ReadBits(flag, 1);
    CanCancel = flag != 0;
    aReader.ReadBits(flag, 1);
    Accepted = flag != 0;
    CellId.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    LeaderName = Serialization::ReadString(aReader);
}
