#include <Messages/NotifyScriptAnimation.h>

void NotifyScriptAnimation::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Cell.Serialize(aWriter);
    Animation.Serialize(aWriter);
    EventName.Serialize(aWriter);
}

void NotifyScriptAnimation::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    Cell.Deserialize(aReader);
    Animation.Deserialize(aReader);
    EventName.Deserialize(aReader);
}
