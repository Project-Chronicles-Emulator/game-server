# Tutorial combat

## Combat rework — 2026-10-26

The [current rework record](combat-rework-20261026.md) supersedes the historical
first-listed NPC selection, universal three-second repeat interval and untargeted
projectile limitations below. It covers unique event identity, free-aim/linear flight
targeting, impact splash, repeated cylinder/box area pulses, range-aware NPC rotation
and the corrected 2249 stage importer. These are local implementation changes;
original-client visual acceptance and the remaining physics/conditional AI scope
are explicitly pending.

## Area spawn hits and area unit ids — 2026-10-27

Live logs showed every cast of the Mage elemental bursts 2274 (lightning) and 2275
(flame) applying only their area's interval effect 143514, which has zero
coefficients, so the bursts never damaged their target. In the official mage
capture each burst places its area (160402/160401) on the StartSkillReq target, and
the damage is the area's `aoeSpawnSkillEffectId` (160505/160503), applied once at
the area's created tick with the area unit as DamageNoti caster. Across the
capture corpus, 113 area spawns with an authored spawn effect show this shape.

- `tools/build_combat_data.py` exports that SkillObject field as the optional area
  key `spawn_effect_id` and adds the effect to the profile. The regenerated
  profiles differ from the reproducible baseline only by 24 such keys and 19
  newly exported effects (31 player skills and 2 NPC skills use these areas).
- `CombatSession::spawn_area` applies the spawn effect right after the area's
  UnitAddNoti, to the enemies inside the area (effect `maxTargetCount` or the area
  cap), before the first interval pulse.
- Area unit ids now fit 32 bits: `0xD0000000 | sequence << 4 | 3`. Captured area
  ids are 32-bit with type nibble 3, and the client resolves unit-addressed packets
  through the low 32 bits of the wire id (see [world-mobs.md](world-mobs.md)). The
  former bit-60 tag left later packets naming the area without a matching actor.

Placement without any skill target (no `target_ids` or lock-on) still uses the
caster position; every captured burst named a target, so official no-target
placement is unknown. Original-client visual acceptance is pending.

## Warrior capture evidence reviewed 2026-10-26

The Warrior tutorial slice R1 is compared against all six earlier captures,
including Wavesword. Its 63 NPC starts cover 42 associations across 12 caster
types. Four associations are new: Scout 190100400, Warder 190700500, Herald
Knight 230240200 and Warden 220210500. Only the Warder and Warden associations
are absent from the reviewed runtime membership input.

The 41 matched NPC-to-player damage notifications contain 36 positive amounts
and five zero contacts. Keep the latter outside damaging-hit timing. Scout's
three distinct grouped effects hit near 502/1,053/1,858 ms; Warder's staff slam
near 1,600 ms; Warden's charging slash near 896 ms. Herald Knight 230240210 has
no overlap group and lands four positive hits in one cast, supplying a concrete
ungrouped multi-hit case for future regression checks. Prowler chain intervals
near 1,704/1,450 ms reinforce Mage observations without establishing AI rules.

Fight levels 1–2 add 35 own longsword/greatsword action IDs, already statically
indexed: 353 requests, 344 own starts, nine reaction-locked start refusals,
374 effect reports and 148 matched ordinary effect refusals. Eleven landing
inputs cover 1000506 and 1051026. Counter 1127 accompanies the first observed
`PS2C_ResetCooldownNoti`, effect 110328; the catalog resets pursuit 1000526 on
self. No current reset notification path was established, and before/after
cooldown readiness, parry gates and general counter rules remain unresolved.

See the eleven tables in the generated
[protocol feature reference](protocol-feature-reference.md) and provenance in
[the capture corpus](capture-corpus.md). This evidence update changes no runtime
profile or behavior and includes no original-client emulator acceptance trial.

## Runtime ownership

`src/world/features/combat/combat.*` owns a character's transient combat state on the existing world
worker strand. The runtime remains C++20 and shares the tutorial socket timer;
there is no extra process, per-character thread, or catalog read during a hit.

`tools/build_combat_data.py` extracts the class/weapon permissions, costs,
cooldowns, action layers, stages, combo/cancel windows, effects, projectiles,
and stat recovery definitions into the tutorial startup profile. The profile
currently indexes 1,008 actions, including observed tutorial NPC actions. A
profile entry is evidence that an action was extracted, not that every special
effect or unlock condition has been reconstructed.

The four starting classes use their own equipped weapon and the shared
`CharacterStats` service. Skills from another class or weapon are rejected.
Item actions remain in the durable tutorial inventory path. Combat does not
consume potions and does not roll loot itself: it reports each defeated monster to an
optional observer, and the loot service described in [monster-loot.md](monster-loot.md)
rolls and grants corpse loot. The optional Bound Prowler quest awards
the door key through its quest reward, after combat supplies the kill event.

## Mage and NPC capture evidence added 2026-10-26

The Mage combat comparison in `docs/protocol-feature-reference.md` adds the M1
capture to T/P1/P2/A1 and keeps all counts in
`protocol/demo-capture-observations.json`. Its tables distinguish NPC casts,
matched NPC hits on the player, own mage requests, nearby players, stage inputs
and client effect reports. Original native tick differences are normalized from
nanoseconds to milliseconds; the runtime's 100 ns ticks are a separate domain.
Notification-arrival delays use the native log clock and are labeled separately.

M1 adds 22 mage request IDs and 22 NPC/skill associations to the comparison
corpus. It gives repeated Prowler chain transitions around 1.70/1.45 s, six
Infantryman 190100300 hits around 1.40 s after cast start, and mage multi-hit
events near 300; 410/630; and 860/960/1,050 ms. These observations do not recover
an AI selection algorithm or justify a universal average attack timer.

Two limitations need particular care before using this evidence in runtime data:
backward evasion 190700800 has catalog type Normal without an established damage
role, and the old 2249 stage-0 effect observation at 608 ms conflicts with its
catalog and M1's stage-1 reports. Distinct unique-event IDs must also survive
multi-hit handling. This analysis does not regenerate combat profiles or prove
local original-client mage or NPC compatibility.

## Wavesword capture evidence added 2026-10-26

The W1 comparison in the protocol reference adds 3,953 complete frames against
the previous five captures. The existing Magic character uses the catalog's
CurvedSword weapon, corresponding to the user's Wavesword label. Twelve own
skill IDs are new to this corpus; all were already statically indexed. There
are 183 own starts, 152 NPC starts across 52 associations and nine caster types,
and 21 separate nearby-player starts.

This recording begins mid-session. Its separately hash-checked pre-start context
supplies the player identity, initial map 9012, level 3, class and existing NPC
identities. Those 12,199 earlier frames are excluded from W1 counts, novelty and
cast-interval samples. Unit removals and map changes invalidate older identities.

All 19 direct NPC hits on the player have matching new-cast timing. The Alpha
Wolf's 362110100 lands around 690 ms; Pallatius's 140640100→110→120→130 chain
has repeated intervals near 1.79/1.99/1.26 s. These are encounter observations,
not a recovered universal cooldown or skill-selection algorithm. W1 adds two
NPC templates (5001285/5001287) and fifteen NPC/skill associations.

The Wavesword's 342 effect reports include contact-window polls and 163 matching
effect-failure responses. Its 129 direct damage notifications each have a
distinct cast/target/effect combination. Preserve separate combo effects while
respecting overlap groups; do not make every contact poll another hit. Strong
slash 2113 lands near 900 ms and follow-up 2145 near 330 ms. Ordinary effect
failure-response parity, conditional execution and full wave/AOE lifecycle
remain separate implementation and original-client validation tasks.

The corpus's first HealNoti belongs to a nearby LongSword player's damage-linked
self-heal shape, not the recording character. Effect 111211 has the static
healHpPerDamage=15 property, but the capture does not establish a positive healed
amount or the cap/rounding formula. New status 2000000 and projectile 100200 also
belong to nearby actors. The protocol reference contains eight detailed W1 tables.

## Authority and execution

- Starting an action checks class, equipped weapon, instance monotonicity,
  current map, position, target eligibility, action cancellation/combo window,
  cooldown, and available HP/SP before charging its costs.
- Server deadlines execute each stage event once, even without a client effect
  report. Stage input requests must match an extracted transition window.
- `PC2S_ChangeSkillStageReq` is validated by its reason against the matching
  extracted notify: `input_condition` windows, `action_stage_condition` entries
  (ground landing, grab outcomes), or the stage's default next stage. Landing
  skills such as 2213 wait in stage 0 for ground contact; the client's
  `ground_landing` report enters stage 1. Window edges accept 250 ms of
  transport skew. Accepted changes are silent to the owner: the three official
  captures carry no stage response for an accepted change and no stage
  notification addressed to the capturing player, so the earlier local
  response/notification pair is no longer sent. A refused change of the current
  action returns `PS2C_ChangeSkillStageRes` with the recovered error code and
  ends that action (`LandingSkillFail` for landings), so a refused continuation
  cannot hold the caster in a 99-second landing wait. A report for an ended or
  older action only returns `Skill_NotUsingSkill`. An early default-stage report
  is deferred to the server deadline, and a late report of an applied
  transition is ignored. Nearby-player stage notifications are not established.

  transport skew. Accepted changes are silent to the owner, as in the captured
  official traffic. A refused change of the current action returns
  `PS2C_ChangeSkillStageRes` with the recovered error code and ends that action
  (`LandingSkillFail` for landings), so a refused continuation cannot hold the
  caster in a 99-second landing wait. A report for an ended or older action
  only returns `Skill_NotUsingSkill`. An early default-stage report is deferred
  to the server deadline, and a late report of an applied transition is ignored.
- Target selection uses local hostile NPCs and extracted shape/range data.
  Inactive scripted NPCs and civilians cannot be damaged. The three Recalling
  Grounds enemies activate when the server quest chain accepts quest 10000043.
- Projectiles follow the captured ownership split. A client-driven player
  projectile (catalog `isServerDriven=false`: staff fireballs, arrows, bolts,
  thrown axes) exists once the client announces it with
  `PC2S_ExecuteSpawnProjectileSkillEffectReq`. The server echoes
  `PS2C_SpawnProjectileNoti` under the client's own instance, which the client
  needs in order to confirm its flight; a refused report gets one
  `PS2C_SpawnClientProjectileFailNoti` per flight. Server-driven (monster)
  flights are numbered by the server from `0x40000000` upward and launch from
  the catalog spawn offset (Candidate).
- The server schedules every impact from distance, speed and lifetime, then
  sends `DamageNoti` (with the flight instance and the Projectile origin) and
  `PS2C_ProjectileHitNoti`. Client hit/update reports cannot change HP or
  accelerate impact. A client despawn report is echoed once, and a reported
  miss withdraws only that flight's own pending hit. Projectile and recovery
  counts are bounded.
- HP changes and deaths are published with the recovered native schemas. A
  monster's death calls `TutorialSession::record_monster_defeat` with the server
  tick. That handler persists quest credit before the resulting packets are
  queued. A failed commit closes the connection. Client requests cannot claim
  a monster defeat.
- `src/world/features/npc/npc_ai.*` owns a transport-free `Idle`/`Engaged`/`Returning` state machine
  for active hostile tutorial monsters. Circle/cone detection and damage acquire
  the local player; friendly, inactive, and dead NPCs do not acquire targets.
  Monsters without a captured NPC skill whose first stage executes an effect
  (currently 5001083, 5001084, 5001085, and 5001529) do not acquire by proximity.
  They still retaliate after damage, but cannot attack until their skills are
  recovered. Engaged NPCs pursue at the extracted run speed, pause movement during
  actions, and use the existing server-deadline skill and damage path. Pursuit stops
  100 mm inside the selected action's effective range, or half that range for very
  small radii, and the attack check accepts the same settled distance.
- Aggro acquisition follows the captured order: `ChangeNpcAggroTopTargetNoti` (98),
  `UnitChangeStateNoti` (32, Combat), then `SetNpcAiPhaseNoti` (114, phase 1230)
  for the ten ordinary tutorial monster types. The Herald Knight's boss phases are
  not emitted. Death follows the captured clear: Demise and death, then 98 and 114
  without target or phase fields. Ordinary pursuit uses `MoveNoti` (177), with Run,
  increasing nanosecond ticks, prediction and a Stop before the attack. Packets 64/106
  are reserved for an actual skill's supported lunge; orphan skill-motion packets do
  not implement walking. See `docs/npc-locomotion-fix-20261026.md`.
- No leash or reset was captured. Leaving the extracted leash is a local model: it
  cancels the NPC's action/projectiles, sends the aggro/phase clears, reports
  `EX5UnitStateType_Return`, and moves the NPC to its authored spawn. After the
  `returnEndTime` delay at home, the NPC restores HP, reports Idle, and may detect
  again. Alert pre-aggro (phase 1220), scripted Recalling Grounds phases, hearing,
  threat, aggro timeouts, and group behavior are not reconstructed.
- NPC retaliation and automatic attacks share the same recovered NPC skill
  membership and extracted action events. The local scheduling model starts the
  first attack after one second and repeats after three seconds. Movement thinks
  run every 100 ms while engaged/returning and perception every 250 ms while idle;
  those cadences are bounded local policy, not captured server timings.

NPC stat definitions in `protocol/tutorial-combat-observations.json` contain
only CMS IDs and numeric stat values for 15 tutorial NPC types. Captured current
HP/SP are replaced with the corresponding maxima. The same versioned input
records relative skill-event timing where the observed client differs from
the exported action data. It excludes player IDs, names, customization, item
instances, credentials, absolute ticks, and official runtime unit IDs. A
capture SHA-256 and the observation-file SHA-256 retain provenance.

Rebuild the profile with `python tools/build_tutorial_data.py`. To explicitly
refresh the observation input from the private capture, run:

```powershell
python tools/build_combat_data.py --capture out/captures/official-full-session-20261024-01/decoded-final.jsonl
python tools/build_tutorial_data.py
```

## Compatibility limits

Damage currently uses the recovered physical/magic coefficients and character
powers, plus fixed damage where present. Armor mitigation, critical hits, perks,
abnormal-state accumulation, collision-aware projectile flight, collision-aware
NPC navigation, and the complete special-effect system remain unresolved. NPC
pursuit is straight-line and does not establish terrain, obstacle, or navmesh
correctness. The local coefficient model is deliberately documented; it is not
an exact official damage formula.
`addStatusEffect`, `spawnAreaOfEffect`, shield/aura/grab and weapon-slot-change
effects are not yet simulated by this increment. Dodge/guard animations and
costs do not yet establish original invulnerability or block behavior.

HP/SP, cooldowns, and living monster HP are transient and reset on reconnect.
Quest kill receipts persist through the quest service. Combat currently plugs
into the existing map-8000 world session; the second area's definitions are
available, but map handoff and resuming combat on map 10117 need the transition
owner. These limits must remain visible when interpreting test results.

The official trace uses nanosecond monotonic ticks (1,000,000 ticks/ms).
Internal tutorial and combat deadlines remain in 100 ns ticks (10,000 ticks/ms);
the server converts its generated monotonic ticks to nanoseconds in outgoing
TimeSync, Pong, combat, and tutorial messages. Incoming movement and potion
animation ticks are forwarded unchanged rather than converted a second time.
The TimeSync/Pong epoch `timestamp` remains in 100 ns units, as verified against
the official captures. Observation extraction still converts official deltas
to milliseconds before scheduling. This wire correction has synthetic coverage;
observer interpolation, original-client animations, and a measured two-client
movement delay still require a live trial.

## Verification

`tools/test_build_combat_data.py` checks capture clock normalization, stage-relative
timing, filtering of the shared area, and removal of runtime/player identifiers.
`Combat.*` tests exercise timer-driven damage, exactly-once quest credit,
forged targets and hit reports, stamina/cooldowns, class/map restrictions,
projectile travel, and recovered-schema encoding. Stage-change tests cover a
2213-shaped landing skill entering its landing stage, its completion freeing
the caster for the next cast, refused reasons/stages/positions ending the
action, stale reports, default-stage reports and input-window timing; an
opt-in test runs the extracted skill 2213. Set
`CHRONICLES_TUTORIAL_DEFINITIONS` to the generated profile to exercise the actual
extracted data and spawn packets. These are offline and synthetic checks;
original-client combat has not yet been verified for this implementation.

projectile travel, automatic aggro, pursuit, leash/reset, and recovered-schema
encoding. Stage-change tests cover a 2213-shaped landing skill entering its
landing stage, its completion freeing the caster for the next cast, refused
reasons/stages/positions ending the action, stale reports, default-stage
reports and input-window timing; an opt-in test runs the extracted skill 2213.
`NpcAi.*` tests cover perception eligibility, damage acquisition,
movement timing and settling, action pauses, and delayed health reset. The
generated-profile TLS scenario also approaches a Skeleton Scout without a player
attack, receives aggro/pursuit/NPC damage, escapes and reacquires after reset,
then completes player combat. Set both `CHRONICLES_TUTORIAL_TEST_PROFILE` and
`CHRONICLES_TUTORIAL_DEFINITIONS` to the generated profile for these opt-in
checks. These are offline and synthetic checks; original-client combat has not
yet been verified for this implementation.

The coordinated 2026-10-24 build passed the seven combat tests and the combined
combat/potion TLS trial under a process-only `OPENSSL_ia32cap=0` diagnostic
fallback. A selected 52-case integration run passed 50 tests and skipped two
optional database/capture checks, with no failures. The normal accelerated
OpenSSL path failed in the combined TLS trial: debugger evidence showed a
nonvolatile XMM6 register holding the test store pointer being clobbered across
a library call. The same preserved failing executable completed the trial with
CPU acceleration disabled. This fallback is for the coordinated local trial;
normal acceleration, exact cause, and performance remain open. See
[the OpenSSL diagnostic record](openssl-local-diagnostic.md).
