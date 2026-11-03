# World monsters

The world population now has a server-owned monster path alongside civilians and private
tutorial/GM encounters. `tools/build_world_mobs.py` exports verified map layers, spawners,
NPC templates, offensive phase-skill trees, actions/effects and supported patrols. The persistent
startup helper generates `data/generated/world-mobs/definitions.json`; the native listener loads
it beside the configured client catalog. Invalid build identities or runtime definitions fail
startup. Generated profiles remain private, ignored build output.

The same-build export contains 2,802 slots on 106 populated maps, including 417 slots on 9012 and
812 on 9031. There are 196 map definitions altogether, 470 supported authored patrols, 941 combat
stat definitions and 1,893 action definitions. These are coverage counts, not official gameplay
fidelity. The export records 154 missing map layers and every exclusion: inactive, time-gated,
missing-selector and missing-point spawners remain explicit. Maps 8000/10117 keep their existing
private tutorial lifecycle.

## Runtime contract

- Public-map clients share monster health, AI/action ownership, deaths and respawns. Matched
  raid channels use independent state keyed by the actual matchmaker channel ID. Catalog maps
  marked `Instance` use a separate population per character; party-instance admission remains
  unrecovered. Housing does
  not attach to public monster populations. Existing connection workers serialize player actions;
  a recursive map mutex serializes shared monster mutations and the notification journal.
- Clients receive current snapshots within a local 150 m interest radius, capped at 1,024 actors.
  A bounded shared journal delivers visual combat notifications every 100 ms. Private replies,
  inventory, loot rights, EXP and quest changes stay private. Falling behind the 4,096-entry
  journal forces a fresh snapshot.
- Existing combat owns costs, deadlines, ranges, projectiles, effects, damage, perception,
  pursuit, retreat, leash and death. The engaged player's worker resolves a monster's attacks;
  another worker cannot repeat those hits against its own player. Any player may damage a public
  monster; the final server-owned hit receives the kill. Disconnect releases the target and resets
  a living monster at home. GM test spawns remain private and do not grant world kill rewards.
- Supported authored patrols run while idle and pause during engagement/return. Roaming-enabled
  NPCs without a path use the explicit bounded local walking policy below. Nubs retain NPC type
  nibble 2 and seed the neutral movement rate when absent. Every life has a random **32-bit wire
  ID**, unique within its channel and outside the ordinary civilian prefix. A separate random
  server-only receipt protects durable quest kill credit across lives/restarts. A respawn cannot
  inherit an old projectile target or corpse loot request.
- The killer's loot service rolls one corpse and grants items through the existing revisioned
  item-trade transaction. Server death events advance active HuntMonster objectives with the
  existing sequential-group rules and award progression's monster EXP. Storage commits finish
  before reward packets are sent. Client kill reports cannot create these events. Character
  progress, inventory and EXP use existing persistence; encounters remain transient.
- Slots respawn at home on their deadlines with full stats and fresh AI/cooldowns. One-time
  slots remain dead until the channel/server is recreated. Shared corpse visibility lasts at most
  90 seconds; the killer's loot service may remove an emptied corpse earlier from that client.
  State is bounded to 16,384 authored slots plus 8,192 retained lives; a full map defers respawn
  until corpses expire instead of dropping existing corpse rights.

## Local policies and remaining fidelity

For dungeon, instance and point-of-interest maps, the initial shared monster population is
attached and announced after the client sends StartPlayingReq, following StartPlayingRes. The
official Cliffs capture announces monster units after that transition. Previously, the emulator
announced them during map loading; a player reported invisible but attackable monsters in
instances. This ordering change is a candidate repair, covered by a synthetic dungeon TLS test;
the original client has not yet confirmed that it fixes the reported rendering problem. Public
field maps retain their existing entry timing.

Spawner maxima are filled, weighted choices are reproducible per exported slot, multi-point
placements cycle through authored points, and respawn uses `spawnDelayMaxMs` (minimum 100 ms).
These are explicit local policies. Group members use authored rotated offsets; linked aggro,
group threat and collective respawn rules are not recovered. Candidate unit conversions and
straight-line locomotion retain the limitations in `tutorial-spawns.md` and
`combat-rework-20261026.md`; this does not implement navigation or collision authority.

`Npc.isRoaming=True` previously had no runtime effect without a named path. Those placements
now get short idle walks, alternating home with two seeded destinations. The radius is at most
3 m or one quarter of the authored leash, whichever is smaller; pauses are 2–4.55 seconds and
the speed comes from the model's walking speed. Stationary models and `isRoaming=False` stay
stationary. The regenerated profile has 687 local roaming slots (163 on 9012), alongside the
470 authored patrols. All 21 Meadow Wolf slots receive local roaming. The fallback keeps spawn
height and has no terrain/navmesh projection, so it is a local approximation rather than an
official wandering algorithm. Combat, return, GM AI pause and death suspend route progress;
respawn and return resume from home rather than a stale route point.

Offensive phase trees contribute action memberships, including nested `nextSkills`. The existing
local range/cooldown rotation selects attacks. Tree weights, chains, phase gates, conditions and
boss scripts are not reproduced by flattening that membership. Every placed type has an extracted
Normal action; this does not certify every effect or original-client animation.

765 NPC types without captured stats use the custom level model in `local_stats`: HP is
150 + 75 × level, physical rating is 10 + 4 × level, and magic rating is 10 + 2 × level. Captured
stats remain preferred. These are playable fallback values, not recovered balance. One placed
type lacks a complete locomotion block and retains damage-triggered legacy behavior. Unsupported
patrols do not become invented routes. Time/quest/event activation, navigation, full threat,
boss phases, party contribution rewards, crash recovery and official balance remain unfinished.

## Validation

The extraction/combat/patrol Python suites pass 105 cases. Focused C++ tests cover shared health,
single kill credit, timed hits, fresh respawn identities, one-time slots, map/interest boundaries,
private GM isolation and independent channels. Release server, main tests and event tests build in
`out/build-world-mobs` with the reused vcpkg cache. Selected CTest runs register 283 cases:
267 pass, 16 optional cases skip and none fail. All eight WorldMobs cases pass, including all-map
profile parsing/encoding and executable offensive-action availability for every hostile type.

The two catalog-backed native TLS tests pass: public clients share a death, EXP persists, only
the killer takes the corpse, a repeated take grants nothing, and Instance-map populations are
private per character. These TLS cases use the documented child-process `OPENSSL_ia32cap=0`
diagnostic; they do not certify default CPU-dispatch TLS. The 251 module cases and 14 raid cases
pass with normal dispatch. This is selected synthetic verification; the full aggregate suite was
not run. See `VALIDATION.md` and ignored `out/world-mobs-*-summary.json` for the results.

The live loopback service was gracefully replaced on 2026-10-26 at 19:43 UTC with the tested
Release artifact (`6f278824d7b197e41c2b2f5ebc927c086cc73734e24abb3b7c275aa6729ebd58`).
Both listeners became healthy, the unchanged PostgreSQL process stayed online, and the server
logged all 196 world-monster map definitions loaded. The matching profile
hash is `cdd70055c05901308566829fd318cbce0fe8b1ec2f995fc61b5ff9215c4a1fe1`.
Private deployment/rollback records are under ignored `out/world-mobs-live-20261026/`.

The initial restart skipped migrations and exposed a missing version-29 dependency in the
candidate's character queries: `nui_blessing_level` and `dedicated_nui_tears`. After a full private
database backup, only the reviewed additive version-29 migration was applied transactionally.
Admin account/character queries then returned HTTP 200 without another restart. Readiness must
include these storage-backed routes, because `/health` alone did not expose the missing columns.

Original-client mob gameplay acceptance is still pending. The client launched before replacement
had an invalidated launch grant (HTTP 401); subsequent launcher queries hit the missing-column
error before repair. No world connection reached the new service during that attempt; the armed
180-second native capture recorded zero frames. A fresh launcher sign-in and Play are required.
Observe spawning, pursuit, hits, corpse loot, quest/EXP changes and respawn before claiming those
checks passed.

## Reported live defects and correction — 2026-10-26

The human observed rendered wolves standing still and NPC attacks landing while player attacks
failed, then no visible monsters after reconnect. The initial private local log decodes 317
frames and includes NPC casts but no player skill start. A later 846-frame snapshot includes
player skills 2220/2249, a stage transition/root motion and one `Skill_UsingSkill` refusal for
2201. These observations do not establish a universal inability to start player skills or
successful player damage.

Static verification found a concrete wire contract violation: `PS2C_MoveNoti` transport at
`0x1461B86C0` calls `0x146092DB0`, which zero-extends only the low dword before actor lookup.
Full-key actor registration therefore cannot match the old random-high-dword mob identity.
The allocator now uses `0x90000002 | random_low_typed_slot`; durable quest receipts keep separate
63-bit server-only identities. Synthetic protobuf round trips had not tested this client lookup.
Explicit player targets also now use the existing live/attackable/private-ownership predicate.

Fifteen inspected functions have repeatable comments, preserved semantic names or aliases,
cached pseudocode and extent/byte fingerprints under
`../dumps/ida_research/world-mob-bugs-20261026/`. The matching IDB was saved successfully with
stored-section verification. The corrected Release server and tests build. Of 268 selected C++
cases, 252 pass and 16 optional cases skip; all 11 WorldMobs cases pass. Both real-catalog TLS
checks pass with the documented portable OpenSSL child setting. Python mob/combat/patrol checks
pass 106 cases. These checks include low-dword ID resolution, separate kill receipts, friendly
target refusal, idle walks and fresh respawns; they do not certify original-client rendering.

The corrected server/profile replaced the live loopback service at 17:13 America/Sao_Paulo.
Both listeners and storage-backed admin routes passed readiness, with PostgreSQL unchanged and
no new migration. Private artifact, profile and rollback hashes are pinned in
`out/mob-bugfix-20261026/running.json`. Corrected original-client movement, targeting, rewards
and respawn still require the reconnect trial against this artifact.

The re-entry timeout was separately traced to a TLS handshake failure after the launcher's
one-connection helper completed. Re-arming the existing guarded helper restored local world
entry and its original code/callback bytes. This is a verified recovery, not acceptance of a
permanent automatic launcher reconnect correction.

## Invisible attackers after reconnect — 2026-10-26

The reconnect trial against the typed-ID correction still failed visually. The private native
log contains 94 complete frames, including NPC attacks from a wolf with no preceding actor
announcement. Initial map load announced civilians, while only later interest changes announced
a few monsters. That log establishes missing spawn delivery independently of rendering.

`WorldSession::receive_gathering` replaced the shared outgoing packet vector with its initial
depleted-node snapshot, erasing the monster `UnitAddNoti` packets queued earlier in map-load
handling. Combat had already marked those actors announced, so subsequent timers could attack
without sending their spawn again. The adapter now appends snapshots, ordinary replies and
broadcast changes without discarding another service's queued work.

The native TLS fixture now accepts gathering data. The dedicated
`GatheringSnapshotPreservesWorldMonsterSpawnsAndRewards` regression enables the real gathering
and mob profiles together, verifies initial announcements for both clients, then checks shared
death, persisted EXP, killer-only loot and duplicate-loot protection. Earlier mob TLS tests had
left gathering disabled and could not catch the production combination. Corrected build,
deployment and original-client results are recorded in `VALIDATION.md`.

The corrected original-client trial rendered the wolves and their combat effect/health bar.
Its private 549-frame log records three kills with EXP increases and successful looting; each
killed placement returns with a fresh ID after about 12 seconds. None of the eight recorded
NPC casts comes from an unannounced actor. Idle walking animation still needs visual acceptance.
The same tested artifact is restored after the subsequent whole local process group exit;
that exit's cause is unknown. See `VALIDATION.md` for recovery and documentation-check limits.
