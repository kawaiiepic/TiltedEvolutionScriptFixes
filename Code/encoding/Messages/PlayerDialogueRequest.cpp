#include <Messages/PlayerDialogueRequest.h>
#include <algorithm>

void PlayerDialogueRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Action, 8);
    aWriter.WriteBits(ActorId, 32);
    aWriter.WriteBits(Token, 32);
    aWriter.WriteBits(Revision, 32);
    aWriter.WriteBits(Index, 32);
    const uint32_t cCount = static_cast<uint32_t>(std::min<size_t>(Options.size(), 64));
    aWriter.WriteBits(cCount, 8);
    for (uint32_t i = 0; i < cCount; ++i)
        Serialization::WriteString(aWriter, Options[i]);
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
    aReader.ReadBits(value, 32);
    Revision = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 32);
    Index = static_cast<uint32_t>(value);
    aReader.ReadBits(value, 8);
    Options.resize(static_cast<size_t>(value));
    for (auto& option : Options)
        option = Serialization::ReadString(aReader);
    Text = Serialization::ReadString(aReader);
}
