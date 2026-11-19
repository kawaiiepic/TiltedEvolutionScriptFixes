#include <Messages/NotifyPlayerMapMarkers.h>
#include <TiltedCore/Serialization.hpp>

#include <algorithm>

void NotifyPlayerMapMarkers::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Serialization::WriteVarInt(aWriter, Tick);
    const size_t count = std::min(Players.size(), MaxPlayers);
    aWriter.WriteBits(count, 8);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& player = Players[i];
        Serialization::WriteVarInt(aWriter, player.Id);
        Serialization::WriteString(aWriter, player.Name);
        player.Cell.Serialize(aWriter);
        player.WorldSpace.Serialize(aWriter);
        player.Entrance.Serialize(aWriter);
        player.EntranceWorldSpace.Serialize(aWriter);
        Serialization::WriteFloat(aWriter, player.Position.x);
        Serialization::WriteFloat(aWriter, player.Position.y);
        Serialization::WriteFloat(aWriter, player.Position.z);
        if (player.Entrance)
        {
            Serialization::WriteFloat(aWriter, player.EntrancePosition.x);
            Serialization::WriteFloat(aWriter, player.EntrancePosition.y);
            Serialization::WriteFloat(aWriter, player.EntrancePosition.z);
        }
    }
}

void NotifyPlayerMapMarkers::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    Tick = Serialization::ReadVarInt(aReader);
    uint64_t count{};
    aReader.ReadBits(count, 8);
    Players.clear();
    if (count > MaxPlayers)
        return;

    Players.resize(static_cast<size_t>(count));
    for (auto& player : Players)
    {
        player.Id = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
        player.Name = Serialization::ReadString(aReader);
        player.Cell.Deserialize(aReader);
        player.WorldSpace.Deserialize(aReader);
        player.Entrance.Deserialize(aReader);
        player.EntranceWorldSpace.Deserialize(aReader);
        player.Position.x = Serialization::ReadFloat(aReader);
        player.Position.y = Serialization::ReadFloat(aReader);
        player.Position.z = Serialization::ReadFloat(aReader);
        if (player.Entrance)
        {
            player.EntrancePosition.x = Serialization::ReadFloat(aReader);
            player.EntrancePosition.y = Serialization::ReadFloat(aReader);
            player.EntrancePosition.z = Serialization::ReadFloat(aReader);
        }
    }
}
