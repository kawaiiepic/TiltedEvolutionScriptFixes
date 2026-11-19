
#include <Interface/Menus/MapMenu.h>

static TiltedPhoques::Initializer s_init(
    []() {
// Disabled because the mapmenu in first person breaks.
// I fix that later, but for now it doesn't break gameplay.
#if 0
    // https://github.com/Vermunds/SkyrimSoulsRE/blob/master/src/Menus/MapMenuEx.cpp
    // Of course this isnt perfect yet. but we"ll see

    VersionDbPtr<void*> hookLoc(53112);
    TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x53, 4);
    TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x9D, 2);
    TiltedPhoques::Nop(hookLoc.GetPtrU() + 0x9F, 1);
#endif
    });

bool MapMenu::IsLocalMapOpen() const noexcept
{
    if (!uiMovie)
        return false;

    struct NumberValue
    {
        void* ObjectInterface{};
        uint32_t Type{0x83}; // GFxValue::kConvertNumber
        uint32_t Padding{};
        double Number{};
    };
    static_assert(sizeof(NumberValue) == 0x18);
    using GetVariable = bool (*)(const void*, NumberValue*, const char*);
    auto* const* pVtable = *static_cast<void***>(uiMovie);
    const auto getVariable = reinterpret_cast<GetVariable>(pVtable[0x11]);

    // SkyUI uses the same movie for its world, local and location-finder views.
    NumberValue state;
    return getVariable(uiMovie, &state, "_root.localMapFader.MapClip._state") &&
           state.Type == 3 && state.Number > 0.0;
}
