#include <Messages/NotifyPlayerDialogue.h>

void NotifyPlayerDialogue::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Kind, 8);
    aWriter.WriteBits(ActorId, 32);
    aWriter.WriteBits(Token, 32);
    aWriter.WriteBits(CanChoose ? 1 : 0, 1);
    Serialization::WriteString(aWriter, Name);
    Serialization::WriteString(aWriter, Text);
}

void NotifyPlayerDialogue::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);

    uint64_t value{};
    aReader.ReadBits(value, 8);
    Kind = static_cast<uint8_t>(value);
    aReader.ReadBits(value, 32);
    ActorId = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 32);
    Token = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 1);
    CanChoose = value != 0;
    Name = Serialization::ReadString(aReader);
    Text = Serialization::ReadString(aReader);
}
