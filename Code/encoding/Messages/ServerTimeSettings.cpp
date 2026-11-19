#include <Messages/ServerTimeSettings.h>

void ServerTimeSettings::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    timeModel.Serialize(aWriter);
    aWriter.WriteBits(RestHours, 5);
}

void ServerTimeSettings::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    timeModel.Deserialize(aReader);
    uint64_t hours{};
    aReader.ReadBits(hours, 5);
    RestHours = static_cast<uint8_t>(hours);
}
