#include <Messages/ScriptAnimationRequest.h>

void ScriptAnimationRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Id.Serialize(aWriter);
    Cell.Serialize(aWriter);
    Animation.Serialize(aWriter);
    EventName.Serialize(aWriter);
}

void ScriptAnimationRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    Id.Deserialize(aReader);
    Cell.Deserialize(aReader);
    Animation.Deserialize(aReader);
    EventName.Deserialize(aReader);
}
