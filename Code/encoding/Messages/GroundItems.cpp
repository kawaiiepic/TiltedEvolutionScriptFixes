#include <Messages/GroundItems.h>
#include <TiltedCore/Serialization.hpp>

void RequestGroundItems::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Cell.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Id);
}

void RequestGroundItems::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Cell.Deserialize(aReader);
    Id = Serialization::ReadVarInt(aReader);
}

void NotifyGroundItem::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Kind, 3);
    Serialization::WriteVarInt(aWriter, Id);
    if (Kind == Spawn || Kind == RejectDrop)
        Serialization::WriteVarInt(aWriter, LocalReference);
    if (Kind == Spawn || Kind == Award)
        Item.Serialize(aWriter);
    if (Kind == Spawn || Kind == SnapshotComplete)
        Cell.Serialize(aWriter);
    if (Kind == Spawn)
    {
        for (int i = 0; i < 3; ++i)
        {
            Serialization::WriteFloat(aWriter, Position[i]);
            Serialization::WriteFloat(aWriter, Rotation[i]);
        }
    }
}

void NotifyGroundItem::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t kind{};
    aReader.ReadBits(kind, 3);
    Kind = static_cast<uint8_t>(kind);
    Id = Serialization::ReadVarInt(aReader);
    if (Kind == Spawn || Kind == RejectDrop)
        LocalReference = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    if (Kind == Spawn || Kind == Award)
        Item.Deserialize(aReader);
    if (Kind == Spawn || Kind == SnapshotComplete)
        Cell.Deserialize(aReader);
    if (Kind == Spawn)
    {
        for (int i = 0; i < 3; ++i)
        {
            Position[i] = Serialization::ReadFloat(aReader);
            Rotation[i] = Serialization::ReadFloat(aReader);
        }
    }
}
