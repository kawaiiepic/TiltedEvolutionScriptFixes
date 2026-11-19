#pragma once

#include <ExtraData/ExtraData.h>

// CommonLibSSE-NG RE/E/ExtraTeleport.h.
struct DoorTeleportData
{
    uint32_t linkedDoor;
    NiPoint3 position;
    NiPoint3 rotation;
    uint32_t flags;
};
static_assert(sizeof(DoorTeleportData) == 0x20);

struct ExtraTeleport : BSExtraData
{
    DoorTeleportData* teleportData;
};
static_assert(offsetof(ExtraTeleport, teleportData) == 0x10);
