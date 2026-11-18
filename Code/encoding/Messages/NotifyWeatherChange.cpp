#include <Messages/NotifyWeatherChange.h>

void NotifyWeatherChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Cell.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Owner);
}

void NotifyWeatherChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    Cell.Deserialize(aReader);
    Owner = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
}
