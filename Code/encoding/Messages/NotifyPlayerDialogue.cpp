#include <Messages/NotifyPlayerDialogue.h>
#include <algorithm>

void NotifyPlayerDialogue::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Kind, 8);
    aWriter.WriteBits(ActorId, 32);
    aWriter.WriteBits(Token, 32);
    aWriter.WriteBits(CanChoose ? 1 : 0, 1);
    aWriter.WriteBits(Revision, 32);
    aWriter.WriteBits(Selected, 32);
    const uint32_t cCount = static_cast<uint32_t>(std::min<size_t>(Options.size(), 64));
    aWriter.WriteBits(cCount, 8);
    for (uint32_t i = 0; i < cCount; ++i)
    {
        Serialization::WriteString(aWriter, Options[i]);
        aWriter.WriteBits(i < Votes.size() ? Votes[i] : 0, 32);
    }
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
    aReader.ReadBits(value, 32);
    Revision = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 32);
    Selected = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 8);
    Options.resize(static_cast<size_t>(value));
    Votes.resize(Options.size());
    for (size_t i = 0; i < Options.size(); ++i)
    {
        Options[i] = Serialization::ReadString(aReader);
        aReader.ReadBits(value, 32);
        Votes[i] = static_cast<uint32_t>(value);
    }
    Name = Serialization::ReadString(aReader);
    Text = Serialization::ReadString(aReader);
}
