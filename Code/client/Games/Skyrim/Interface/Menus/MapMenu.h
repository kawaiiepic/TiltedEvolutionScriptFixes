#pragma once

#include <Camera/TESCamera.h>
#include <Games/Events.h>
#include <Interface/IMenu.h>

class MenuOpenCloseEvent;
struct TESWorldSpace;

struct IMapCameraCallbacks
{
    // add
    virtual void Unk_00(void); // 00
    virtual void Unk_01(void); // 01

    virtual ~IMapCameraCallbacks(); // 02
};
static_assert(sizeof(IMapCameraCallbacks) == 0x8);

struct MapMenu final : public IMenu, public BSTEventSink<MenuOpenCloseEvent>, public IMapCameraCallbacks
{
    TESCamera& GetWorldCamera() noexcept { return worldCamera; }
    TESWorldSpace* GetMapWorldSpace() const noexcept { return worldSpace; }
    bool IsLocalMapOpen() const noexcept;

    // CommonLibSSE-NG RE/M/MapMenu.h, SE/AE layout.
    uint8_t pad40[0x304A0 - 0x40];
    TESCamera worldCamera;
    uint8_t padCamera[0x90 - sizeof(TESCamera)];
    uint64_t unk30530;
    TESWorldSpace* worldSpace;
};
static_assert(offsetof(MapMenu, worldCamera) == 0x304A0);
static_assert(offsetof(MapMenu, worldSpace) == 0x30538);
