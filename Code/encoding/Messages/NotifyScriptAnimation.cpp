#include <Messages/NotifyScriptAnimation.h>

void NotifyScriptAnimation::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Cell.Serialize(aWriter);
    Serialization::WriteVarInt(aWriter, Tick);
    EventName.Serialize(aWriter);
}

void NotifyScriptAnimation::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    Cell.Deserialize(aReader);
    Tick = Serialization::ReadVarInt(aReader);
    EventName.Deserialize(aReader);
}
