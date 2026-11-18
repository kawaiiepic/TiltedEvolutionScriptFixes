# Shared campaign roadmap

The dedicated server records shared quest events. Skyrim still evaluates conditions, runs Papyrus fragments, holds each character's save and renders its journal and HUD. Quest updates, scene observations and shared dialogue work independently of party membership. The protocol handshake checks the source commit so clients and the server must use matching builds.

## Status by phase

| Phase | Implemented on this branch | Remaining work and validation |
| --- | --- | --- |
| 1. Campaign foundation | Versioned, flushed journal with plugin-backed keys, v1 support, a durable stage-and-stop record, duplicate and stale-stop rejection, server snapshots, bounded replay and acknowledgements, per-quest policy, console status and backup. | Replay starts from the first record on each connection because an older save can be loaded. Client snapshot application and compaction need a native save watermark and proof that skipping fragments preserves their effects. Native completion is recorded separately in v3 journals. Failure and abandonment still need distinct verified native transitions. Reset needs a verified native lifecycle and a generation boundary. |
| 2. Native quest synchronization | Clients use the existing native stage path and hold stages around local scene conditions. Scene observation samples placed reference aliases and reports divergence. Native completion observations are replayed through the existing completion wrapper after the requisite stage and local scenes finish. | Validate stage, journal and marker behavior on both supported Skyrim runtimes. The source declares objective layouts and exposes CompleteAllObjectives, but has no per-objective writer or event that supplies stable IDs and HUD invalidation. Failure has a readable flag but no verified native setter. Quest properties, local variables and location aliases need verified read/write and per-player scope. Dynamic targets and repeatable or radiant generations need stable reference identity and a verified fill/reset point before dependent fragments run. |
| 3. Dialogue | Nearby option and vote panels, temporary native response grants, voluntary handoff, disconnect migration, per-session privacy, optional distant read-only panels, and owner-scoped voice/subtitle relays. | Exercise handoff and privacy with two clients. Lines emitted before the grant stay local. Public conversation voice and subtitles follow the same nearby range as the shared dialogue panel. Spectating mirrors options and vote totals. Native branch replay and persuasion, intimidation and bribery outcomes require a supported native choice operation with stable topic identity and outcome timing. |
| 4. Scripted scenes | Native scene observers publish phase and action state, with timeout/disconnect migration, per-recipient alias filtering and preference for the owner of a matching public conversation who observes the scene. Stage delivery respects the existing scene guards. | Scene state is observed, and phase transitions still run locally. Scene creation, guaranteed completion, association when there is no matching placed actor alias, and script side-effect deduplication need verified native scene lifecycle and action identity. An active server snapshot cannot start a missing follower scene. |
| 5. World persistence | Placed locks persist by reference policy, cell policy or recognized NoResetZone. NoResetZone interiors and configured cells retain plain container stock. Eligible placed NPCs retain recorded death or resurrection automatically based on the actor base Respawn flag. Scripted animations use plugin IDs and stay within the actor or object cell. | Verify native restore with two clients. Door activation can execute scripts or teleport, and the available wrapper only reads its open state. Spawn, deletion, quest item ownership and general reset-aware persistence require provenance that existing packets lack. Faction updates carry the current actor ownership epoch. The server includes its accepted faction snapshot in ownership handoffs. Transient activation and lock notices are scoped to their original cell. |
| 6. Rewards | Combat skill XP stays with the individual by default on new configurations. Existing servers can retain party combat XP through the existing setting. | Shared gold, artifacts, eligibility, and transaction recovery need a verified grant path with durable grant identity in the character save. A server acknowledgement alone cannot establish that an inventory transaction survived a client crash. Existing Papyrus rewards still run through Skyrim. |
| 7. Multiplayer | Audited actor handoff and inventory epoch checks. Fixed scene handoffs, scoped stale object replies to the original cell, and added ownership epochs to movement and actor refresh packets. Exterior weather now uses a server-selected source per occupied cell and releases its override on departure. | Regional weather boundaries beyond a cell need an exposed native climate/region identifier. Combat, AI, movement, animation, magic, summons, projectiles, stealth, respawn and boss changes need live replication traces and tests. |
| 8. Native experience | Quest updates go through the native quest path. New controls apply to voting, privacy and dialogue ownership. Journal tracking and multiplayer status require in-game checks. | Native journal and HUD divergence stays possible if the local fragment or alias setup differs. Status is available to the server operator through the console. |
| 9. Mod compatibility | Plugin and light-plugin identities, quest overrides, fallback diagnostics and a read-only Lua snapshot with a post-commit event. | Clients require compatible plugin versions. Plugin filenames must match server and policy spelling. Test altered load orders, missing plugins, and both SKSE runtime combinations with a modded quest. |
| 10. Validation | Tests exercise packet round trips, v1 replay, malformed and torn records, duplicate stages and stops, persistence failures, policy precedence, snapshots, and lock recovery. | A Windows client build and two playable Skyrim clients are needed for the lifecycle cases below. Profile live bandwidth and event processing under real saves. |

The existing feature branch derived quest state from native start, stage and stop observations. This preserves authored conditions but also means a script can run locally before the server learns of it. A stop or stage accepted by the server cannot undo already executed effects on another save. The server serializes accepted records as it receives them, suppresses duplicate stages and refuses to reopen a stopped quest. It has no quest-specific knowledge of whether two distinct stages represent mutually exclusive choices.

## Native dialogue presentation

Resolved non-actor speakers (including talking activators) now use native local dialogue without requesting an NPC ownership grant. Any previous shared grant is cleared when entering local dialogue. Unresolved speakers still require a verified actor identity before shared selection. Test Alternate Start's Statue of Mara while connected and confirm normal NPC ownership remains enforced.

Native non-owner option selections already count as votes when the text uniquely matches an owner option. The desired presentation is the existing Skyrim dialogue list with vote counts appended to display labels, not a second voting list. The repository's menu wrapper exposes an opaque `uiMovie` pointer and read-only option text, but no verified Scaleform dialogue-list update method or lifetime contract. Do not overwrite native option text pointers or mutate interned strings. Verify the actual Dialogue Menu list and refresh path for both supported runtimes before replacing the overlay voting list. Preserve original option identity, revision validation, privacy and owner-only execution. The existing overlay remains until that native display path can be verified.

### Character portraits and native vote labels

Nearby public conversation snapshots now include bounded server-player/option pairs along with aggregate votes. Remote spectator snapshots expose only counts. Private sessions expose no voter identities. The existing fallback panel displays a count suffix and voter-name tooltip, not fabricated portraits. Native non-owner selections continue matching unmodified option text.

[Smart Talk's dialogue renderer](https://github.com/Seb263/SkyrimSE_SmartTalk/blob/master/src/Features/Quest.hpp) demonstrates a Scaleform entry callback that preserves the original renderer and accesses the entry's topic index and text field. The fork still needs a verified GFx ABI, thread-safe refresh for votes while the list is open, and compatibility with modded dialogue menus. [Mesh Rendering Framework](https://github.com/QTR-Modding/Mesh-Rendering-Framework/blob/master/include/MeshRenderingFrameworkAPI.h) exposes an actor mesh constructor and image export API backed by CommonLib types; use an optional ABI-safe companion or verified native integration to render and cache actual character portraits, not generic race icons. Validate runtime support and GPL licensing before adopting source. The original network change did not implement native labels.

An optional portrait implementation now consumes the documented [Phantasm still-rendering API](https://github.com/Severause/Phantasm/blob/main/api/Phantasm_API.h) (MIT, vendored header, blob `122aa6b4fff851a3765014e08e4d4a797809fbe6`). It discovers the already-loaded `Phantasm.dll` only while there are nearby public voters, checks interface size/version, resolves the voter to the local replicated actor or player, and requests one 96x96 portrait at a time. The browser caches up to 32 locally generated PNG data URLs; active portraits refresh periodically and reset on disconnect. Missing plugin, unloaded actors or unavailable pictures leave vote counts intact. No portrait images go over multiplayer transport. Install Phantasm and its dependencies separately for the matching Skyrim runtime. The optional client integration is source-only until compiled and tested with both supported runtimes and a two-client conversation. Native label/portrait placement in the Scaleform list remains blocked by verified list refresh, texture registration, and menu lifetime contracts; the current portrait placement is in the fallback panel.

## Server-owned characters and login

The connection protocol is being split into password/version authentication (character lobby) and explicit native world entry. Lobby clients reserve a server slot for up to 30 minutes but have no gameplay Player, admin session, actor or campaign replay. Entry supplies the character name, loaded plugin list and initial cell only when the engine is ready; plugin policy applies before creating the Player. Existing in-world connect requests automatically proceed through both stages. Main-menu users must load or create their character and explicitly enter.

The desired persistence model is **server-owned saves with client-side Skyrim simulation**. The server should maintain an opaque persistent server identity, authenticated account and character UUID, a single active character lease, and an atomic canonical checkpoint revision containing the paired native ESS and SKSE co-save, a plugin manifest and shared campaign sequence. Download and verify the latest revision into an isolated client staging directory before native loading; upload a completed checkpoint with its expected base revision and lease, validate bounds/checksums, commit both files together, retain rollback backups, and reject stale or conflicting uploads. Never use a character display name, claimed Discord ID or shared server password alone as account identity. The client still executes the native engine, Papyrus, mods and save generation. Server-owned save selection prevents normal stale-save entry but does not make a trusted client cheat-proof. Native file save/load completion, creation flow, path isolation, account credentials, upload/download, and enforceable revision checks are not implemented by the login split. Do not invent save-manager offsets; verify native integration on both supported runtimes.

### Experimental checkpoint backend

The authenticated lobby now has a bounded checkpoint protocol (`RequestCharacterSave` / `NotifyCharacterSave`), using a private 32-byte random bearer capability per character slot. The server stores only a SHA-256-derived slot directory name, not the capability, with exclusive per-connection leases, immutable numbered revisions, strict base-revision and contiguous offset checks, combined ESS/co-save size limit (128 MiB), digest validation, durable staging and publication, and best-effort retention of the previous revision. Initial saves may be uploaded in the lobby; returning characters must download the latest before entering; subsequent uploads require world entry. The server cannot prove the client actually loaded the bytes or prevent edited saves. The protocol is disabled by default: `GameServer:bRequireServerSaves` is for headless/experimental clients only until native save/load automation and a character-selection UI exist. One server writer per directory is required. Packet transfers are bounded but disk IO and commit hashing are synchronous in this first backend; tune/measure before production. Account registry, secure capability generation/storage, manifest and campaign watermark, automatic native save/load, and character list are pending. Existing saves are never automatically imported.

The main-menu overlay now labels its entry Play and opens the connect view directly on F2. Native New/Load remain available for the existing lobby workflow: replacing them needs the verified native menu renderer/action path, not a guessed relocation or blocking the user's ability to create a character.

## Server operation

Use a separate working directory for each server instance. Keep the files below on a persistent volume when using a container and use one active writer for that directory.

### Quest policies

Create `shared-quest-policies.conf` beside `shared-quests.log` in the server working directory. Use `shared` or `personal`, then `standard` or `light`, the exact plugin filename and the hexadecimal base FormID without the load-order prefix. Restart to load changes. An invalid file disables shared quest and scene processing until corrected.

```text
# Example policies for an installed quest mod
personal standard "Example Adventure.esp" 0x00A123
shared light "Example Side Quest.esl" 0x8AB
```

Without an override, most quest types are shared. Set `bEnableMiscQuestSync` under `[Gameplay]` to change the default for miscellaneous and hidden quests. Client safety exclusions for repeatable, dynamic and known personal quests still apply. An override to personal also filters history when a client reconnects. Re-enabling an override allows previous shared history to replay again.

### Persistent locks

Create `shared-lock-policies.conf` with the same syntax. A reference policy overrides `shared-lock-cell-policies.conf` and the recognized vanilla NoResetZone default for eligible interior cells. Both policy files use the same syntax. Cell policies take the placed cell’s base FormID. If home synchronization is disabled, its native exclusions still apply. List the placed reference's base FormID, using the plugin that defines the reference and only selecting doors or containers intended to remain in the player's final state. Runtime-created references and locks that reset during normal gameplay should stay outside this file.

```text
shared standard "Example Adventure.esp" 0x014A82
```

Accepted changes write `shared-locks.log`. Restoring a lock does not restore a door's open animation or the inventory of a container. A malformed log disables persistent writes for opted-in locks. Preserve that file for inspection before replacing it with a known-good backup with the server stopped.

### Persistent NPC life and container stock

Death and resurrection are recorded automatically for placed, plugin-backed NPCs whose base has Respawn disabled, with essential and templated bases excluded from the automatic path. The server excludes players, runtime-created actors, mounts, player summons and arrivals identified as having a leveled base even if the selected form cannot be sent. The optional `shared-npc-life-policies.conf` can mark individual placed references as `personal` when a script manages their lifecycle or opt in a stable placement. Owner-confirmed transitions go to `shared-npc-life.log`. They are restored through the existing native kill and resurrection paths.

Ordinary container stock persists automatically in recognized NoResetZone interiors that pass the existing player-home setting and personal stash exclusions. The optional `shared-stock-cell-policies.conf` selects additional cells or marks them `personal` to disable stock persistence. Plain plugin-backed stacks go to `shared-stock.log`. When saved stock exists, both the first and subsequent clients reconcile plain stacks while preserving native quest and instance-specific entries. A missing plugin's entries remain in the journal until a compatible client loads them. Containers with no committed stock change still use their native baseline. Inventory transfers are executed on the client, so this mode cannot guarantee exactly-once loot across simultaneous takes or rolled-back character saves.

### Recovery and administration

- Run `/campaign-status` in the dedicated server console for journal health, the affected record line, loaded policies and replay cursors.
- Run `/campaign-backup` to create `shared-quests.log.backup` from the committed journal. Archive an existing backup under a distinct name before creating the next one. Back up the policy and lock files alongside the quest journal.
- To recover the quest journal, stop the server, preserve the damaged file, restore a known-good backup as `shared-quests.log`, then restart and check `/campaign-status`. A torn final line is reported even if it contains all fields. v1 and v2 quest journals remain readable. New records use v3 and older binaries cannot read them.
- A replay acknowledgement means the client quest service processed the batch. It does not certify that pending native fragments ran or that the character was saved. Recover affected characters from corresponding saves when rolling back a campaign.
- `bRemoteDialogueSpectating` under `[Gameplay]` exposes public panels to distant players. Only nearby players can vote on those panels. Owners can make a conversation private and pass control to an eligible player who already has the native conversation open.
- Set `bEnableXpSync` under `[Gameplay]` to enable party combat XP sharing. An existing INI keeps its configured value.

## Lua resources

`QuestService.get():GetCampaignQuest(plugin, light, baseFormId)` returns `nil` when no quest is recorded. Otherwise it returns a copy with `sequence`, `highestStage`, `stopped`, `completed` and a numerically sorted `stages` array. The array contains explicitly recorded stages and a final stage inferred from a stop when applicable. Sequence numbers refer to records in the local campaign journal. Historical data is still queryable after an administrator makes a quest personal.

```lua
addEventHandler("onCampaignQuestCommitted", function(sequence, plugin, light, baseFormId, stage, kind, playerId)
    local quest = QuestService.get():GetCampaignQuest(plugin, light, baseFormId)
    -- kind is 0 for a stage, 1 for a start, 2 for a stop and 3 for completion.
    if quest.stopped then
        print("Campaign quest stopped at stage " .. stage)
    end
end)
```

The event fires after a successful durable append. Reconnect replay and journal recovery do not repeat it.

## Interactive object animations

Scripted `ObjectReference.PlayAnimation` and `PlayAnimationAndWait` now relay accepted named animation events for placed non-actor references. The client replays the start of the animation through the ordinary non-latent `PlayAnimation` path. The original wait event and Papyrus stack belong to the caller. Reliable events retain the object and cell plugin IDs and the sender's clock tick. The observer briefly waits for the object's 3D and plays events in order for that reference. Leaving the cell drops pending events.

Live actor action playback now passes the native target event and target idle to the existing mediator at the actor's playback tick. Action, idle and target references resolve through each player's load order. Furniture playback avoids a second `Activate` call. The animation override suppresses synchronous echoes from replay and nested script wrappers. Historical actor replay leaves furniture target data out so reconnecting cannot restart an old interaction.

The native behavior graph still determines what a named event starts, stops or loops. This implementation carries observed transition events and leaves the graph to run its own transitions. It cannot seek a loop to its current phase or reconstruct an in-progress loop for a late observer. The repo has no verified capture hook for arbitrary graph dispatches, the separate `Debug.SendAnimationEvent` and `PlaySyncedAnimation*SS` Papyrus natives, or Skyrim-only temporary furniture references. Those paths and any Papyrus animations started asynchronously by a behavior-graph callback need runtime traces before broadening coverage. The mod's public description establishes its use of context-aware animation and deferred activation. It does not identify the native bellows path. [Vanilla ObjectReference API](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/scripts/vanilla/ObjectReference.psc) · [Immersive Interactions by JaySerpa](https://www.nexusmods.com/skyrimspecialedition/mods/47670)

The Linux server at `0eefbb5a` built and passed 210 assertions in 23 cases. The loopback pair verified ordered start, loop and stop event names with their original ticks and modded object reference, rejected an empty name and an out-of-cell request, and observed no sender echo. These checks exercised transport and server filtering. Native client playback remains unverified.

Validate a vanilla forge and a forge with Immersive Interactions using two Skyrim clients on matched builds. Verify the bellows when the actor approaches, enters, stays in the loop, leaves or cancels, and repeats the interaction. Use different compatible light-plugin load orders for a modded furniture reference. Test a cell transition while an event is pending and verify that the remote observer receives only the visual action and never starts crafting or runs a second activation script.

## Shared carriage travel

Players travelling together activate the same carriage passenger seat. When one rider completes the native trip, the server sends eligible passengers at the departure cell to that player's actual destination. Other players retain control of their travel, and parties are optional. Only the originating ride runs the fare and departure scripts. Boarding expires after 60 seconds, and the server checks that each rider is still within 512 game units of the departure seat. A boarding token prevents a duplicate or stale arrival from moving someone again. Ordinary map travel clears the local boarding record.

The client recognizes the vanilla `isCartTravelPlayer` furniture keyword. It uses the existing fast-travel completion dispatcher and also accepts a distant same-worldspace arrival after observing disabled movement and a loading screen from a recorded boarding. The fallback covers scripted travel paths that may omit the native completion event. The implementation uses the existing player teleport destination resolver. Custom carriages that lack the keyword are outside this path. [Vanilla carriage keyword](https://cslist.uesp.net/index.php?game=sr&formid=0x00103444) · [Vanilla CarriageSystemScript](https://cslist.uesp.net/index.php?game=sr&script=CarriageSystemScript)

The Linux server at `b384823a` built and passed 220 assertions in 24 cases. Loopback clients verified partyless carriage arrival, one matching passenger teleport, no teleport for a bystander or a different carriage, and rejection of stale and repeated arrival packets. The Windows client and native ride remain unverified.

Two-client Skyrim validation should cover one player buying a Whiterun-to-Riften trip, a second player activating the same carriage, and a third player staying near the driver without boarding. Check the second player's camera and controls after arrival. Also try cancellation, ordinary map fast travel and two players buying their own trips. The scripted loading fallback and the furniture-to-arrival timing require an in-game run.

## Player revival and respawn

With the multiplayer death system enabled, a downed player can be revived in place. A standing player in the same cell can press **F10** near them. This works without a party. The server accepts one recovery outcome per downed episode and checks that the helper is neither the downed player nor downed themselves. Revival carries no gold penalty.

If nobody revives the player before `Gameplay:uPlayerRespawnDelay` (30 seconds by default), the existing respawn path becomes available automatically. The default `Gameplay:fGoldLossFactor` is `0.10`, with a minimum loss of one gold when the player has any. Values above `1.0` specify flat gold loss. An existing INI keeps its configured value, so an existing value of `0` leaves gold loss disabled.

The Linux server at `ede745d3` built and passed 230 assertions in 25 cases. Loopback clients verified a partyless revive, one fallback gold deduction, and rejection of distant, self, early, stale and duplicate requests.

The player client uses the existing bleedout recovery and respawn routines. Verify revive-in-place, recovery from ragdoll, observer animation, and the gold loss displayed by the Skyrim client with two game clients. Test a revive arriving when the fallback timer expires and confirm that one recovery applies.

## Validation checklist

Run `xmake build TPTests` and `xmake run TPTests`. Build the Linux server and Windows client with the matching branch. Build the overlay with the pinned lockfile and pnpm 9. Each replay batch has at most 16 updates and only one batch can be outstanding per player. The server retries an unacknowledged batch after two seconds. Public dialogue panels send on change, and unchanged lock state does not append another disk record.

The Linux dedicated server at `8c9c74f5` built successfully. `TPTests` passed 206 assertions in 22 cases. Headless clients verified protocol rejection, partyless quest completion replay with acknowledgement retry, plugin remapping, Lua snapshot recovery, and torn-journal detection. They also verified automatic NoResetZone lock recovery, configured container stock recovery, configured NPC death and resurrection across restarts, and exterior weather handoff without a party. These checks exercise the server and wire protocol. Native Skyrim application, client compatibility, and gameplay remain pending. The Windows workflow reported no checks on this branch, and its manual dispatch returned HTTP 404.

The checked-in Linux transport test can run without Skyrim.

```sh
xmake build SkyrimServerRunner
xmake build CampaignSmokeClient
python3 Code/tests/integration/campaign_smoke.py \
  build/linux/x86_64/releasedbg/SkyrimTogetherServer \
  build/linux/x86_64/releasedbg/CampaignSmokeClient build/BuildInfo.h
```

The test uses the generated handshake, a temporary server directory, and loopback clients with no party. Linux CI runs it on amd64. The Windows workflow now supports manual dispatch and runs `TPTests`, including when called by the two-runtime playable workflow.

The Linux server built at `d49018e6`. `TPTests` passed 210 assertions in 23 cases, including faction epoch encoding and bounds. The loopback test verified automatic NoResetZone lock and stock recovery, concurrent stock assignment, automatic NPC death and resurrection across restarts, exclusion of respawning and leveled NPCs, and the personal override. It also exercised quest completion replay and retry, missing and remapped plugins, journal diagnostics and weather handoff. Console and Resources tests and workflow lint passed. The GitHub fork still reported zero registered workflows and returned HTTP 404 on dispatch, so Windows compilation and in-game tests remain unverified.

Use a dedicated server and two game clients to validate these cases before calling the campaign playable end to end.

1. Start and progress the same quest concurrently with no party. Reconnect one client on an older save and restart the server between sessions. Inspect native objectives, markers, fragments and rewards on both clients.
2. Use a placed modded door and container with different compatible load orders. Unlock them, let both clients leave, then restart the server and inspect the first client's locks and inventory.
3. Verify persisted NPC death and resurrection with old saves, and a container with an empty committed stock, a quest item and a client missing an item plugin. Check that nearby partyless clients share exterior weather and that an interior transition releases any override.
4. Open the same conversation, change options, vote, pass control and disconnect the owner. Make the conversation private and confirm that its panel, native voices and subtitles stay local. Inspect native scene progress while both clients play the same scene.
5. Compare actor ownership and combat logs during cell changes, owner disconnects and simultaneous NPC or object interactions. Collect server tick timings, replay volume and client frame timings on a long-running campaign.

The engine-dependent changes use the hook IDs and types already present in the repository. No Skyrim binary inspection or two-client runtime was available for this work. In particular, the repository's existing stop path clears quest flags and marks the form changed. Native cleanup of aliases, fragment side effects, quest status semantics and scope boundaries require verification before adding stronger lifecycle guarantees.

The current development pass uses the source-declared `TESQuest::SetCompleted`, `Actor::Kill`, `Actor::Resurrect(false)`, lock and inventory wrappers, and the existing native weather override. Engine behavior and callback ordering for these paths still need a two-client test. The engine audit identified the following implementation paths and open verification.

- Vanilla `Quest.psc` declares the three objective setters and their getters, `Reset`, `Stop` and latent `Start` and `SetCurrentStageID`. The local registry collects untyped callback addresses, and the exposed VM interface does not declare an instance dispatch wrapper. The existing SetCurrentStageID path now looks up its registration on use and guards an unavailable VM. CommonLibSSE documents handle-based method dispatch as a candidate. Verify the matching vtable, handle and arguments ABI, stack suspension and return handling on both supported runtimes before using it. Objective changes also need an observation point that includes changes made by native code and stage fragments. [Vanilla Quest script](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/scripts/vanilla/Quest.psc) · [CommonLibSSE VM dispatch](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/include/RE/I/IVirtualMachine.h)
- SKSE exposes alias enumeration, and vanilla script APIs expose reference and location alias reads and writes. The existing native path resolves reference aliases by ID. A portable radiant instance still requires a verified hook between alias filling and execution of startup fragments, matching locations and dynamic references across saves, and a way to distinguish a reset from a delayed event for the old instance. The local `currentInstanceID` field has no verified cross-save meaning. Repeatable and radiant replay stays excluded where restart would replay or overwrite independent state. [SKSE quest bindings](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PapyrusQuest.cpp) · [ReferenceAlias script](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/scripts/vanilla/ReferenceAlias.psc) · [LocationAlias API](https://ck.uesp.net/wiki/LocationAlias_Script)
- The existing menu detour accepts a native option index into a local option list. `TESTopicInfoEvent` observes the line begin and end after selection, and Papyrus `Topic.Add` exposes topic discovery. Replaying a choice on every client could run quest fragments and give rewards again. A pre-execution choice identity and a verified way to observe the resolved response and its outcome are needed. [Vanilla Topic script](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/scripts/vanilla/Topic.psc)
- The SKSE Papyrus plug-in API registers native functions through a callback. That API supplies no supported C++ call path for these vanilla instance methods. SKSE runtime discovery now matches the exact filenames used by its loader and checks the startup export before reporting success. Leave SKSE optional. [SKSE plugin API](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64/PluginAPI.h) · [SKSE runtime naming](https://github.com/ianpatt/skse64/blob/25b72352adb6543fa6d0bd3795780672b2e238e0/skse64_loader_common/IdentifyEXE.cpp)
- The non-respawning NPC default uses the respawn bit already described by the repository's NPC record decoder and independently documented for the runtime actor base field. Scripted resets, persistent factions after all clients unload, reward grant attribution and recovery, door state without activation, and general quest-wide world consequences remain open. `AddOrRemoveItem` provides no character-save transaction identity, and blindly repeating a grant after a disconnect can duplicate it. [Runtime actor base flags](https://github.com/CharmedBaryon/CommonLibSSE-NG/blob/b93280e832f263dbef44e44cbe2936622a02f91a/include/RE/T/TESActorBaseData.h)
