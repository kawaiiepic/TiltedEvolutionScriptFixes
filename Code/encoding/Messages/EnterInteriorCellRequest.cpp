#include <Messages/EnterInteriorCellRequest.h>
#include <TiltedCore/Serialization.hpp>

void EnterInteriorCellRequest::SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept
{
    CellId.Serialize(aWriter);
    SourceDoor.Serialize(aWriter);
    if (SourceDoor)
    {
        Serialization::WriteFloat(aWriter, SourceDoorPosition.x);
        Serialization::WriteFloat(aWriter, SourceDoorPosition.y);
        Serialization::WriteFloat(aWriter, SourceDoorPosition.z);
    }
}

void EnterInteriorCellRequest::DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept
{
    ClientMessage::DeserializeRaw(aReader);

    CellId.Deserialize(aReader);
    SourceDoor.Deserialize(aReader);
    if (SourceDoor)
    {
        SourceDoorPosition.x = Serialization::ReadFloat(aReader);
        SourceDoorPosition.y = Serialization::ReadFloat(aReader);
        SourceDoorPosition.z = Serialization::ReadFloat(aReader);
    }
    else
        SourceDoorPosition = glm::vec3(0.f);
}
