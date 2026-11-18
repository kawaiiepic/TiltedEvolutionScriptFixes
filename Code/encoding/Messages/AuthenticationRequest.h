#pragma once

#include "Message.h"
#include <Structs/Mods.h>
#include <TiltedCore/Buffer.hpp>
#include <Structs/GameId.h>
#include <Structs/TimeModel.h>

struct AuthenticationRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kAuthenticationRequest;

    AuthenticationRequest()
        : ClientMessage(Opcode)
    {
    }

    virtual ~AuthenticationRequest() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const AuthenticationRequest& achRhs) const noexcept
    {
        return GetOpcode() == achRhs.GetOpcode() && DiscordId == achRhs.DiscordId && SKSEActive == achRhs.SKSEActive && MO2Active == achRhs.MO2Active && Token == achRhs.Token && Version == achRhs.Version && UserMods == achRhs.UserMods && Username == achRhs.Username &&
               WorldSpaceId == achRhs.WorldSpaceId && CellId == achRhs.CellId && Level == achRhs.Level
            && PlayerTime == achRhs.PlayerTime && Phase == achRhs.Phase;
    }

    enum PhaseCode : uint8_t { Login, EnterWorld };
    // Login authenticates a connection, not a character. Only EnterWorld
    // carries native character and plugin data.
    uint8_t Phase{Login};
    uint64_t DiscordId{};
    bool SKSEActive{};
    bool MO2Active{};
    String Token{};
    String Version{};
    Mods UserMods{};
    String Username{};
    GameId WorldSpaceId{};
    GameId CellId{};
    uint16_t Level{};
    TimeModel PlayerTime{};
};
