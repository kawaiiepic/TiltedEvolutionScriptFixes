# Skyrim Together plugin messaging

SKSE plug-ins can use the existing Skyrim Together connection to exchange binary messages with other installed add-ons. The server treats the payload as opaque and attaches the authenticated sender's session player ID. No script resource or extra port is required.

Include [PluginMessagingAPI.h](Code/client/PluginMessagingAPI.h) and resolve the versioned export from the Skyrim Together launcher at runtime.

```cpp
#include <Windows.h>
#include "PluginMessagingAPI.h"

const SkyrimTogether::PluginMessagingInterface* FindMessaging()
{
    auto module = GetModuleHandleW(L"SkyrimTogether.exe");
    if (!module)
        module = GetModuleHandleW(nullptr);

    const auto query = reinterpret_cast<SkyrimTogether::QueryPluginMessaging>(
        GetProcAddress(module, SkyrimTogether::PluginMessagingExport));
    if (!query)
        return nullptr;

    const SkyrimTogether::PluginMessagingInterface* api = nullptr;
    return query(SkyrimTogether::PluginMessagingVersion, &api) ==
                   SkyrimTogether::PluginMessageResult::Ok ? api : nullptr;
}
```

- Register one callback for a namespaced channel such as `example.interactions.v1`. A channel accepts up to 64 ASCII characters. Use letters, digits, dots, underscores and hyphens.
- Call `Send` on the game window thread. Pass a session player ID, `Nearby` for Skyrim Together's replication range, or `Broadcast` for all other loaded players. Each reliable message accepts up to 4 KiB. The server rate-limits each sender. A successful send means the local transport accepted the message.
- Receive callbacks run on the game window thread. Copy the payload inside the callback if you need it later. Unregister on the game thread before unloading callback code or freeing its context.
- Use `LocalPlayerId` and `ResolvePlayerFormId` on the game window thread. A FormID of zero means the player is unavailable locally. Resolve the proxy again when needed because it can change when the player leaves range or reconnects.

## Immersive Interactions

The built-in adapter forwards successful standalone Papyrus `Actor.PlayIdle` calls using idle forms owned by `ImmersiveInteractions.esp`. It resolves those forms through the normal load-order mapping and replays them on the sender's loaded proxy. Install the mod and its animation dependencies on participating clients.

This adapter covers the idle playback path. Paired or target-driven actions, script effects and animation replacer conditions that apply only to the local player still need separate integration. In-game validation is pending.

[Immersive Interactions – Animated Actions](https://www.nexusmods.com/skyrimspecialedition/mods/47670)


## RaceMenu appearance

Skyrim Together captures the local player's effective RaceMenu body slider values
and sends them through its native messaging channel. Receivers apply each
snapshot to the sender's loaded proxy using a dedicated RaceMenu morph key.
Snapshots are resent for late arrivals and reapplied when a proxy's 3D reloads.
Slider removals clear the corresponding synced values.

Participating clients need RaceMenu, compatible meshes and the bundled
`STCheckpointBridge.dll` in `Data/SKSE/Plugins`. The bridge discovers RaceMenu's
public SKEE interfaces. No MorphSyncTogether DLL or external relay is
required. The existing Skyrim Together face and tint replication remains active.
The same snapshot carries standard body, hand, foot and face overlays, including
tattoo texture paths, tint and opacity. Clients must have the matching texture
assets installed. Third-person node position, rotation, scale and scale mode are
also synchronized. Synced transforms use their own keys on the remote proxy.

Facial sculpt geometry remains pending. RaceMenu's full preset application also
rewrites facial data and clears existing overrides and transforms. This
implementation uses the narrower overlay and transform APIs. In-game
verification is pending.

[RaceMenu SKEE interface](https://github.com/expired6978/SKSE64Plugins/blob/master/skee64/IPluginInterface.h)
