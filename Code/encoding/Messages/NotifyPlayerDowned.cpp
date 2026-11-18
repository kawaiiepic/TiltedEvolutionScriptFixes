#include <Messages/NotifyPlayerDowned.h>

void NotifyPlayerDowned::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, ActorId);
    Serialization::WriteVarInt(aWriter, DownId);
}

void NotifyPlayerDowned::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    ActorId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    DownId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
