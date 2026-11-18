# Shared campaign roadmap

The dedicated server records shared quest events. Skyrim still evaluates conditions, runs Papyrus fragments, holds each character's save and renders its journal and HUD. Quest updates, scene observations and shared dialogue work independently of party membership. The protocol handshake checks the source commit so clients and the server must use matching builds.

## Status by phase

| Phase | Implemented on this branch | Remaining work and validation |
| --- | --- | --- |
| 1. Campaign foundation | Versioned, flushed journal with plugin-backed keys, v1 support, a durable stage-and-stop record, duplicate and stale-stop rejection, server snapshots, bounded replay and acknowledgements, per-quest policy, console status and backup. | Replay starts from the first record on each connection because an older save can be loaded. Client snapshot application and compaction need a native save watermark and proof that skipping fragments preserves their effects. Completion, failure and abandonment remain indistinguishable from ordinary stops on the wire. Reset needs a verified native lifecycle and a generation boundary. |
| 2. Native quest synchronization | Clients use the existing native stage path and hold stages around local scene conditions. Scene observation samples placed reference aliases and reports divergence. | Validate stage, journal and marker behavior on both supported Skyrim runtimes. Direct objective updates need verified display, completion and HUD invalidation paths. Quest properties, local variables and location aliases need verified read/write and per-player scope. Dynamic targets and repeatable or radiant generations need stable reference identity and a verified fill/reset point before dependent fragments run. |
| 3. Dialogue | Nearby option and vote panels, temporary native response grants, voluntary handoff, disconnect migration, per-session privacy, optional distant read-only panels, and owner-scoped voice/subtitle relays. | Exercise handoff and privacy with two clients. Lines emitted before the grant stay local. Spectating mirrors options and vote totals. Native branch replay, listening at a distance, and persuasion, intimidation and bribery outcomes require verified topic identity, outcome and subtitle lifecycle hooks. |
| 4. Scripted scenes | Native scene observers publish phase and action state, with timeout/disconnect migration and per-recipient alias filtering. Stage delivery respects the existing scene guards. | Scene state is observed, and phase transitions still run locally. Scene creation, guaranteed completion, owner association with a conversation, and script side-effect deduplication need verified native scene lifecycle and action identity. An active server snapshot cannot start a missing follower scene. |
| 5. World persistence | Explicitly configured placed door and container lock states survive cell unload and server restart. The first registered client receives the saved lock without replacing its container inventory. | Validate locking and unlocking with multiple Skyrim clients, including cell leave and restart. NPC lifecycle, inventory and quest items, open doors, switches, puzzles, activation, destruction, factions, Civil War consequences and dungeon respawns need stable event sources and a way to distinguish campaign changes from cell resets and player-local effects. |
| 6. Rewards | Combat skill XP stays with the individual by default on new configurations. Existing servers can retain party combat XP through the existing setting. | Shared gold, artifacts, eligibility, and transaction recovery need a verified grant path with durable grant identity in the character save. A server acknowledgement alone cannot establish that an inventory transaction survived a client crash. Existing Papyrus rewards still run through Skyrim. |
| 7. Multiplayer | Audited actor handoff and inventory epoch checks. Fixed arrival/departure state for dialogue, object mapping cleanup on disconnect and calendar rebasing on reconnect. | Weather still follows the existing party model. Combat, AI, movement, animation, magic, summons, projectiles, stealth, respawn and boss changes need targeted live replication traces and tests. |
| 8. Native experience | Quest updates go through the native quest path. New controls apply to voting, privacy and dialogue ownership. Journal tracking and multiplayer status require in-game checks. | Native journal and HUD divergence stays possible if the local fragment or alias setup differs. Status is available to the server operator through the console. |
| 9. Mod compatibility | Plugin and light-plugin identities, quest overrides, fallback diagnostics and a read-only Lua snapshot with a post-commit event. | Clients require compatible plugin versions. Plugin filenames must match server and policy spelling. Test altered load orders, missing plugins, and both SKSE runtime combinations with a modded quest. |
| 10. Validation | Tests exercise packet round trips, v1 replay, malformed and torn records, duplicate stages and stops, persistence failures, policy precedence, snapshots, and lock recovery. | A Windows client build and two playable Skyrim clients are needed for the lifecycle cases below. Profile live bandwidth and event processing under real saves. |

The existing feature branch derived quest state from native start, stage and stop observations. This preserves authored conditions but also means a script can run locally before the server learns of it. A stop or stage accepted by the server cannot undo already executed effects on another save. The server serializes accepted records as it receives them, suppresses duplicate stages and refuses to reopen a stopped quest. It has no quest-specific knowledge of whether two distinct stages represent mutually exclusive choices.

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

Create `shared-lock-policies.conf` with the same syntax. With no matching `shared` entry, the lock uses the ordinary transient object sync. List the placed reference's base FormID, using the plugin that defines the reference and only selecting doors or containers intended to remain in the player's final state. Runtime-created references and locks that reset during normal gameplay should stay outside this file.

```text
shared standard "Example Adventure.esp" 0x014A82
```

Accepted changes write `shared-locks.log`. Restoring a lock does not restore a door's open animation or the inventory of a container. A malformed log disables persistent writes for opted-in locks. Preserve that file for inspection before replacing it with a known-good backup with the server stopped.

### Recovery and administration

- Run `/campaign-status` in the dedicated server console for journal health, the affected record line, loaded policies and replay cursors.
- Run `/campaign-backup` to create `shared-quests.log.backup` from the committed journal. Archive an existing backup under a distinct name before creating the next one. Back up the policy and lock files alongside the quest journal.
- To recover the quest journal, stop the server, preserve the damaged file, restore a known-good backup as `shared-quests.log`, then restart and check `/campaign-status`. A torn final line is reported even if it contains all fields. A v1 journal stays readable and new records use v2. Avoid running an older server binary against a journal containing v2 records.
- A replay acknowledgement means the client quest service processed the batch. It does not certify that pending native fragments ran or that the character was saved. Recover affected characters from corresponding saves when rolling back a campaign.
- `bRemoteDialogueSpectating` under `[Gameplay]` exposes public panels to distant players. Only nearby players can vote on those panels. Owners can make a conversation private and pass control to an eligible player who already has the native conversation open.
- Set `bEnableXpSync` under `[Gameplay]` to enable party combat XP sharing. An existing INI keeps its configured value.

## Lua resources

`QuestService.get():GetCampaignQuest(plugin, light, baseFormId)` returns `nil` when no quest is recorded. Otherwise it returns a copy with `sequence`, `highestStage`, `stopped` and a numerically sorted `stages` array. The array contains explicitly recorded stages and a final stage inferred from a stop when applicable. Sequence numbers refer to records in the local campaign journal. Historical data is still queryable after an administrator makes a quest personal.

```lua
addEventHandler("onCampaignQuestCommitted", function(sequence, plugin, light, baseFormId, stage, kind, playerId)
    local quest = QuestService.get():GetCampaignQuest(plugin, light, baseFormId)
    -- kind is 0 for a stage, 1 for a start and 2 for a stop.
    if quest.stopped then
        print("Campaign quest stopped at stage " .. stage)
    end
end)
```

The event fires after a successful durable append. Reconnect replay and journal recovery do not repeat it.

## Validation checklist

Run `xmake build TPTests` and `xmake run TPTests`. Build the Linux server and Windows client with the matching branch. Build the overlay with the pinned lockfile and pnpm 9. Each replay batch has at most 16 updates and only one batch can be outstanding per player. The server retries an unacknowledged batch after two seconds. Public dialogue panels send on change, and unchanged lock state does not append another disk record.

Use a dedicated server and two game clients to validate these cases before calling the campaign playable end to end.

1. Start and progress the same quest concurrently with no party. Reconnect one client on an older save and restart the server between sessions. Inspect native objectives, markers, fragments and rewards on both clients.
2. Use a placed modded door and container with different compatible load orders. Unlock them, let both clients leave, then restart the server and inspect the first client's locks and inventory.
3. Open the same conversation, change options, vote, pass control and disconnect the owner. Make the conversation private and confirm that its panel, native voices and subtitles stay local. Inspect native scene progress while both clients play the same scene.
4. Compare actor ownership and combat logs during cell changes, owner disconnects and simultaneous NPC or object interactions. Collect server tick timings, replay volume and client frame timings on a long-running campaign.

The engine-dependent changes use the hook IDs and types already present in the repository. No Skyrim binary inspection or two-client runtime was available for this work. In particular, the repository's existing stop path clears quest flags and marks the form changed. Native cleanup of aliases, fragment side effects, quest status semantics and scope boundaries require verification before adding stronger lifecycle guarantees.
