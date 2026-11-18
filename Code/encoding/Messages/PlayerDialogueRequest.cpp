#include <Messages/PlayerDialogueRequest.h>

void PlayerDialogueRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    aWriter.WriteBits(ActorId, 32);
    aWriter.WriteBits(Token, 32);
    Serialization::WriteString(aWriter, Text);
}

void PlayerDialogueRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    uint64_t value{};
    aReader.ReadBits(value, 8);
    Action = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 32);
    ActorId = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 32);
    Token = static_cast<uint32_t>(value);
    Text = Serialization::ReadString(aReader);
}
