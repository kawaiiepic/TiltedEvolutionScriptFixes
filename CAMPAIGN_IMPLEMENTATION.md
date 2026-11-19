# Campaign implementation checklist

Baseline is master at `92a8552`. Source, automated builds, and gameplay verification are tracked separately. Skyrim 1.7.104 is the primary runtime, with 1.6.1170 retained.

| Status | Requirement and evidence |
| --- | --- |
| IMPLEMENTED | Circular 1px portraits and matching fallbacks across player HUD, player list, character picker and saved previews, and dialogue participants and suggestions. Shared class in `Code/skyrim_ui/src/styles.scss` at `20347ff`. Future travel portraits should reuse the same style. |
| BLOCKED | Near-identical custom clone of Skyrim's option list. Existing native list stays active. A distinct replacement would still need verified option identity, list-only visibility, layout/scroll tracking, safe keyboard/mouse/controller forwarding, and restoration for disconnect, stale CEF, and conflicting SWFs. Preserve native camera, effects, subtitles, voice, scripts, and lifecycle. |
| IMPLEMENTED | Same-worldspace public discovery and explicit remote listen/leave, option snapshots, suggestion lifecycle, and subtitle fallback via server dialogue sessions, existing protocol and CEF. Server `OverlayService.cpp` validates grants and revokes them on region changes, privacy and departure. Remote registration grants suggestions only. Angular exposes Listen and Stop listening in the overlay. |
| PENDING | Remote voice for unloaded speakers needs non-spatial playback, interruption, missing/modded asset handling, and duplicate suppression. Nearby shared native presentation and remote camera need verified lifecycle without opening duplicate native conversations or forcing cell loads. |
| IMPLEMENTED | Existing native owner grant, advisory nearby suggestions, voluntary handoff and disconnect transfer, privacy and compatible location filtering. Runtime regression validation remains due. |
| PENDING | Character settings for auto-join, remote notices and auto-listen, portraits and cinematic presentation. Avoid interrupting gameplay and provide a native-compatible notification path. |
| IMPLEMENTED | Native checkpoint, durable final revision confirmation, retry on missed commit, failed save actions, Windows WM_CLOSE and native main-menu cleanup in `CharacterCheckpoints.cpp`, `TransportService.cpp`, and `World.cpp` at `92a8552`. |
| PENDING | Two-client validation on 1.7.104 and 1.6.1170 for checkpoint file pairing, failure/timeout recovery, in-game Quit, loss and reconnect, character switching and staging. Preserve autosave triggers and avoid uploads on dead transports. |
| PENDING | Individual quest objectives and markers, including display, hide, failure, completion and target changes. Verify TESQuest objective identity/state and HUD/journal invalidation, then add durable replay with suppression. |
| PENDING | Quest reference, actor and location aliases, dynamic target and clear/replacement handling before dependent fragments. Resolve stable placed IDs and generated identity, preserve player-specific aliases. |
| PENDING | Scene execution, phases/actions, positioning, conversations, cancellation, completion, ownership repair and late join. Preserve native scene conditions and once-only side effects. |
| PENDING | Durable per-character and unique quest reward transactions, gold/items/artifacts/experience, eligibility, rollback and replay recovery through committed checkpoint revisions. |
| PENDING | Radiant and repeatable quest start/reset/complete generations, stable target and location remapping, persistent state and reconnect. Preserve unsupported and personal quest exclusions. |
| PENDING | Stable native dialogue choice and outcome, authoritatively relay persuasion, intimidation, bribery, skill checks, gold and relationships while isolating personal effects. |
| PENDING | Server travel invitation state, ten-second countdown, accept/decline/change/cancel, expiry, disconnection and partial failures, destination validation, idempotent transition acknowledgements and arrival coordination. |
| PENDING | Native fast travel, carriage, boat, dungeon/interior/worldspace and modded or scripted transport. Preserve native restrictions, fares, quest fragments, safe arrivals, and one campaign time advance. Existing carriage arrival relay is narrower. |
| PENDING | Exclude non-gameplay moves including save loads, respawns, arrests and corrections. Support follow-up invitations where transitions cannot be safely delayed. |
| PENDING | Crime and arrest isolation, surrender, fines, jail and escape, confiscation/stolen inventory and shared guard/world effects without shared bounties. |
| PENDING | Persistent reset-aware dungeon puzzle states, activation deduplication, recovery and late joins without script or reward replay. |
| PENDING | Durable shared map discovery with load-order-stable identities, replay, fast travel eligibility, hidden markers and duplicate reward/notification suppression. |
| PENDING | Coordinated sleeping and waiting with unanimous approval, timeout and one calendar advance, isolating native rest and survival effects. |
| PENDING | Native map player markers, character names, worldspace and interior entrances, custom waypoints and live zoom-safe updates. Native compass markers where supported. |
| PENDING | Regression testing for serialization, persistence and migrations, quests/rewards/radiant generations, privacy/eligibility/suggestion lifecycle, travel, shared time, puzzles, discoveries, crime, reconnects and checkpoint failures. |
| PENDING | Cross-runtime gameplay, performance and mod compatibility. Preserve global partyless campaign and individual characters, persistent NPCs/containers/locks, trading, proximity voice, revival, nameplates, waypoints, animation, HUD and Phantasm. |
| IN PROGRESS | Incremental master pushes, Windows and Linux CI, Angular production builds and preview release workflow. Baseline preview run 44469160804. |

Resume next from remote voice playback research and a transport-level conversation test for privacy, stale revisions and worldspace transitions.
