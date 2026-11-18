#include <Messages/ScriptAnimationRequest.h>

void ScriptAnimationRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Cell.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Tick);
    EventName.Serialize(aWriter);
}

void ScriptAnimationRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    Cell.Deserialize(aReader);
    Tick = Serialization::ReadVarInt(aReader);
    EventName.Deserialize(aReader);
}
