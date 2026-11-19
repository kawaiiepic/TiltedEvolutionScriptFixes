#include <Messages/RequestFastTravelCatchUp.h>
#include <TiltedCore/Serialization.hpp>

void RequestFastTravelCatchUp::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Sequence);
    Origin.Serialize(aWriter);
    Destination.Serialize(aWriter);
    Position.Serialize(aWriter);
}

void RequestFastTravelCatchUp::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Sequence = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    Origin.Deserialize(aReader);
    Destination.Deserialize(aReader);
    Position.Deserialize(aReader);
}
