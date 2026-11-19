#pragma once

#include "Message.h"
#include <Structs/GameId.h>

using TiltedPhoques::String;

struct EnterInteriorCellRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kEnterInteriorCellRequest;

    EnterInteriorCellRequest()
        : ClientMessage(Opcode)
    {
    }

    virtual ~EnterInteriorCellRequest() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const EnterInteriorCellRequest& acRhs) const noexcept { return CellId == acRhs.CellId && SourceDoor == acRhs.SourceDoor && (!SourceDoor || SourceDoorPosition == acRhs.SourceDoorPosition) && GetOpcode() == acRhs.GetOpcode(); }

    GameId CellId{};
    GameId SourceDoor{};
    glm::vec3 SourceDoorPosition{0.f};
};
