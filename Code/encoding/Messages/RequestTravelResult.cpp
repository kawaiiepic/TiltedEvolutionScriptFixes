#include <Messages/RequestTravelResult.h>
#include <TiltedCore/Serialization.hpp>

void RequestTravelResult::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Id);
    aWriter.WriteBits(Outcome, 8);
}

void RequestTravelResult::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Id = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    uint64_t outcome{};
    aReader.ReadBits(outcome, 8);
    Outcome = static_cast<uint8_t>(outcome);
}
