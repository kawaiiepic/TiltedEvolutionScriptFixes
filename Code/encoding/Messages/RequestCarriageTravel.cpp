#include <Messages/RequestCarriageTravel.h>

void RequestCarriageTravel::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Seat.Serialize(aWriter);
    Cell.Serialize(aWriter);
    Position.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Token);
    Serialization::WriteBool(aWriter, Arrived);
}

void RequestCarriageTravel::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Seat.Deserialize(aReader);
    Cell.Deserialize(aReader);
    Position.Deserialize(aReader);
    Token = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    Arrived = Serialization::ReadBool(aReader);
}
