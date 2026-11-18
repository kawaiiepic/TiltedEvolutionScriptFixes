#include <Messages/RequestWeatherChange.h>

void RequestWeatherChange::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Cell.Serialize(aWriter);
}

void RequestWeatherChange::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    Cell.Deserialize(aReader);
}
