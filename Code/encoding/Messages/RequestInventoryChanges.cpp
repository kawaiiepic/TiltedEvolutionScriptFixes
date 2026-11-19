#include <Messages/RequestInventoryChanges.h>
#include <TiltedCore/Serialization.hpp>

void RequestInventoryChanges::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ServerId);
    Serialization::WriteVarInt(aWriter, OwnershipEpoch);
    Item.Serialize(aWriter);
    Serialization::WriteBool(aWriter, Drop);
    Serialization::WriteBool(aWriter, UpdateClients);
    Serialization::WriteVarInt(aWriter, GroundReference);
    if (GroundReference)
    {
        GroundCell.Serialize(aWriter);
        for (int i = 0; i < 3; ++i)
        {
            Serialization::WriteFloat(aWriter, GroundPosition[i]);
            Serialization::WriteFloat(aWriter, GroundRotation[i]);
        }
    }
}

void RequestInventoryChanges::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    ServerId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    OwnershipEpoch = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Item.Deserialize(aReader);
    Drop = Serialization::ReadBool(aReader);
    UpdateClients = Serialization::ReadBool(aReader);
    GroundReference = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    if (GroundReference)
    {
        GroundCell.Deserialize(aReader);
        for (int i = 0; i < 3; ++i)
        {
            GroundPosition[i] = Serialization::ReadFloat(aReader);
            GroundRotation[i] = Serialization::ReadFloat(aReader);
        }
    }
}
