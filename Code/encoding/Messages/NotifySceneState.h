#pragma once

#include <Messages/Message.h>
#include <Structs/SceneState.h>

struct NotifySceneState final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySceneState;
    NotifySceneState() : ServerMessage(Opcode) {}
    SceneState State{};
    uint32_t Owner{};
    uint32_t Generation{};

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override
    {
        State.Serialize(aWriter);
        aWriter.WriteBits(Owner, 32);
        aWriter.WriteBits(Generation, 32);
    }

    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ServerMessage::DeserializeRaw(aReader);
        State.Deserialize(aReader);
        uint64_t value{};
        aReader.ReadBits(value, 32);
        Owner = static_cast<uint32_t>(value);
        aReader.ReadBits(value, 32);
        Generation = static_cast<uint32_t>(value);
    }

    bool operator==(const NotifySceneState& acRhs) const noexcept
    {
        return State == acRhs.State && Owner == acRhs.Owner && Generation == acRhs.Generation;
    }
};
