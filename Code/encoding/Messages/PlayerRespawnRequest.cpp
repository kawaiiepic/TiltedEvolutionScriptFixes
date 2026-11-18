#include <Messages/PlayerRespawnRequest.h>

void PlayerRespawnRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, DownId);
    aWriter.WriteBits(Action, 8);
    Serialization::WriteVarInt(aWriter, TargetId);
}

void PlayerRespawnRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    DownId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
    uint64_t action{};
    aReader.ReadBits(action, 8);
    Action = static_cast<uint8_t>(action);
    TargetId = Serialization::ReadVarInt(aReader) & 0xFFFFFFFF;
}
