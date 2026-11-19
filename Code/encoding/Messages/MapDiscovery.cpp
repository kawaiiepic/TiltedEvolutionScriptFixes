#include <Messages/MapDiscovery.h>
#include <TiltedCore/Serialization.hpp>
#include <algorithm>

void RequestMapDiscovery::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    Marker.Serialize(aWriter);
}

void RequestMapDiscovery::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);
    Marker.Deserialize(aReader);
}

void NotifyMapDiscoveries::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    const size_t count = std::min(Markers.size(), MaxMarkers);
    aWriter.WriteBits(count, 8);
    for (size_t i = 0; i < count; ++i)
        Markers[i].Serialize(aWriter);
}

void NotifyMapDiscoveries::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ServerMessage::DeserializeRaw(aReader);
    uint64_t count{};
    aReader.ReadBits(count, 8);
    if (count > MaxMarkers)
        return;
    Markers.resize(static_cast<size_t>(count));
    for (auto& marker : Markers)
        marker.Deserialize(aReader);
}
