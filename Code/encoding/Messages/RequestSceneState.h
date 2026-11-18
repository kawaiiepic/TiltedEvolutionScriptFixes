#pragma once

#include <Messages/Message.h>
#include <Structs/SceneState.h>

struct RequestSceneState final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestSceneState;
    RequestSceneState() : ClientMessage(Opcode) {}
    SceneState State{};

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override { State.Serialize(aWriter); }
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override
    {
        ClientMessage::DeserializeRaw(aReader);
        State.Deserialize(aReader);
    }

    bool operator==(const RequestSceneState& acRhs) const noexcept { return State == acRhs.State; }
};
