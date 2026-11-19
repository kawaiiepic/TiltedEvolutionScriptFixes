#pragma once

#include <Forms/TESForm.h>

#include <Components/TESFullName.h>

struct TESWorldSpace : TESForm
{
    // aX and aY are coordinates, not positions
    TESObjectCELL* LoadCell(int32_t aXCoordinate, int32_t aYCoordinate) noexcept;

    TESFullName fullName;
    // CommonLibSSE-NG RE/T/TESWorldSpace.h.
    uint8_t pad30[0x88 - 0x30];
    TESObjectCELL* persistentCell;
};

static_assert(offsetof(TESWorldSpace, persistentCell) == 0x88);
