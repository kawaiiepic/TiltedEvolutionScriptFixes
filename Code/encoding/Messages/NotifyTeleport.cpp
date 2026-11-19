#include <Messages/NotifyTeleport.h>
#include <TiltedCore/Serialization.hpp>

void NotifyTeleport::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    CellId.Serialize(aWriter);
    Position.Serialize(aWriter);
    WorldSpaceId.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, CarriageToken);
    Serialization::WriteVarInt(aWriter, GroupTravelId);
}

void NotifyTeleport::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    CellId.Deserialize(aReader);
    Position.Deserialize(aReader);
    WorldSpaceId.Deserialize(aReader);
    CarriageToken = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    GroupTravelId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
