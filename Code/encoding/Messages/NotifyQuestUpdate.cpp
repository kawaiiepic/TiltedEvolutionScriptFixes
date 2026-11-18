#include <Messages/NotifyQuestUpdate.h>
#include <algorithm>

void NotifyQuestUpdate::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    aWriter.WriteBits(Through, 64);
    const size_t cSize = std::min(Updates.size(), kMaxUpdates);
    aWriter.WriteBits(cSize, 8);
    for (size_t i = 0; i < cSize; ++i)
    {
        const auto& update = Updates[i];
        update.Id.Serialize(aWriter);
        aWriter.WriteBits(update.Stage, 16);
        aWriter.WriteBits(update.Status, 8);
    }
}

void NotifyQuestUpdate::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t value{};
    if (!aReader.ReadBits(Through, 64) || !aReader.ReadBits(value, 8) || value > kMaxUpdates)
        return;

    Updates.resize(static_cast<size_t>(value));
    for (auto& update : Updates)
    {
        update.Id.Deserialize(aReader);
        if (!aReader.ReadBits(value, 16))
        {
            Updates.clear();
            return;
        }
        update.Stage = static_cast<uint16_t>(value);
        if (!aReader.ReadBits(value, 8) || value > SharedQuestUpdate::Completed)
        {
            Updates.clear();
            return;
        }
        update.Status = static_cast<uint8_t>(value);
    }
}
