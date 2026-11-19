#pragma once

#include <ExtraData/ExtraData.h>
#include <Components/TESFullName.h>

// Source layout: CommonLibSSE-NG RE/E/ExtraMapMarker.h.
struct MapMarkerData
{
    static constexpr uint8_t Visible = 1 << 0;
    static constexpr uint8_t CanTravelTo = 1 << 1;
    static constexpr uint8_t ShowAllHidden = 1 << 2;

    TESFullName locationName;
    uint8_t flags;
    uint8_t pad11;
    uint16_t type;
    uint32_t pad14;
};
static_assert(offsetof(MapMarkerData, flags) == 0x10);
static_assert(sizeof(MapMarkerData) == 0x18);

struct ExtraMapMarker : BSExtraData
{
    MapMarkerData* mapData;
};
static_assert(offsetof(ExtraMapMarker, mapData) == 0x10);
static_assert(sizeof(ExtraMapMarker) == 0x18);
