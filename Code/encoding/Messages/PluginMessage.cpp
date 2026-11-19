#include <Messages/PluginMessage.h>
#include <TiltedCore/Serialization.hpp>
#include <algorithm>

bool PluginMessagePayload::IsValidChannel(std::string_view aChannel) noexcept
{
    return aChannel.size() <= MaxChannelLength && aChannel.find('.') != aChannel.npos &&
        !aChannel.starts_with('.') && !aChannel.ends_with('.') &&
        std::all_of(aChannel.begin(), aChannel.end(), [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        });
}

void PluginMessagePayload::Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t channelSize = std::min(Channel.size(), MaxChannelLength);
    const size_t payloadSize = std::min(Bytes.size(), MaxPayloadSize);
    aWriter.WriteBits(channelSize, 8);
    aWriter.WriteBits(payloadSize, 16);
    if (channelSize)
        aWriter.WriteBytes(reinterpret_cast<const uint8_t*>(Channel.data()), channelSize);
    if (payloadSize)
        aWriter.WriteBytes(Bytes.data(), payloadSize);
}

void PluginMessagePayload::Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    uint64_t channelSize{}, payloadSize{};
    aReader.ReadBits(channelSize, 8);
    aReader.ReadBits(payloadSize, 16);
    Channel.clear();
    Bytes.clear();
    if (!channelSize || channelSize > MaxChannelLength || payloadSize > MaxPayloadSize)
        return;

    Channel.resize(static_cast<size_t>(channelSize));
    Bytes.resize(static_cast<size_t>(payloadSize));
    aReader.ReadBytes(reinterpret_cast<uint8_t*>(Channel.data()), Channel.size());
    if (!Bytes.empty())
        aReader.ReadBytes(Bytes.data(), Bytes.size());
}

void RequestPluginMessage::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Target);
    Payload.Serialize(aWriter);
}

void RequestPluginMessage::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Target = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    Payload.Deserialize(aReader);
}

void NotifyPluginMessage::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Sender);
    Payload.Serialize(aWriter);
}

void NotifyPluginMessage::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Sender = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
    Payload.Deserialize(aReader);
}
