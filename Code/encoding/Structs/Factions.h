#pragma once

#include "Faction.h"

using TiltedPhoques::Vector;

struct Factions
{
    Factions() = default;
    ~Factions() = default;

    bool operator==(const Factions& acRhs) const noexcept;
    bool operator!=(const Factions& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader);

    Vector<Faction> NpcFactions;
    Vector<Faction> ExtraFactions;
};

struct FactionUpdate
{
    uint32_t OwnershipEpoch{};
    Factions Content;

    bool operator==(const FactionUpdate&) const noexcept = default;
};
