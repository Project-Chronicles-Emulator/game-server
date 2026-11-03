# NPC locomotion repair — 2026-10-26

The reported behavior is NPCs standing/attacking or appearing to teleport instead of
walking/running in tutorial and open-world trials. This record distinguishes repaired source
contracts from an original-client acceptance trial. The running service has not been switched.

## Causes and changes

- Ordinary pursuit emitted skill-only packets 64/106 with an invented instance that had no
  StartSkill lifecycle. Rechecked client handlers require an active matching skill; they cannot
  implement ordinary walking. Pursuit now emits only MoveNoti 177 and Stop. Authored attack
  lunges retain their real StartSkill -> root motion -> motion end -> EndSkill path.
- The pursuit trajectory used catalog run speed while its notification selected Walk. It now
  selects Run, preserving pred_pos, pred_milli and strictly increasing nanosecond ticks.
  The initial pose faces the route immediately. Target loss/casting settles an active trajectory
  at the interruption tick before stopping or starting a homeward route.
- GM-created fightable NPCs previously received no NpcAi and fell back to stationary legacy
  attacks. They now require an executable attack, verified profile stats and a complete
  catalog-derived locomotion definition. No default chase speed or artificial movement skill
  is inserted for unsupported definitions.
- The combat generator exports 71 complete NPC locomotion blocks using the existing tutorial
  extraction utility. Three passive catalog types have zero range and zero cone angle; an
  inactive cone is accepted without inventing perception. A positive cone range with zero angle
  still fails validation. Unsupported definitions retain an explicit diagnostic table.
  Range/speed conversion and the neutral speed multiplier retain the existing tutorial
  importer's Candidate assumptions; this is not a measurement of official per-model speed.
- GM-created encounters now execute the same private combat controller on open-world maps.
  Units are already visible only to the issuing GM; movement, attacks, projectiles and death
  remain local to that connection. The map is server-bound and cannot change through a client
  request. No public NPC, merchant or another player's private spawn becomes a valid target.
  Authored tutorial quest kills exclude GM units, and despawn cancels their simulation and
  releases the retained actor state.

This repair does not add public shared-world monster authority. Separate authored town-patrol
work can move ambient map actors, but shared monster aggro, damage/loot, respawn and multi-player
combat remain open. Private GM test encounters must not be described as that implementation.

## Client evidence

Same input SHA-256: `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`;
native-log build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`, image base `0x140000000`.
Eight existing functions were rechecked and annotated while preserving existing names/comments.
The IDB saved successfully in place with stored-section verification and a backup. Cached
pseudocode, function extents, first-32-byte fingerprints and append-only annotation records
are under `../dumps/ida_research/npc-locomotion-20261026/`.

The normal-motion client can snap corrections within 500 mm, requires a prediction goal and
at least 16 ms prediction for its moving branch, and derives speed from consecutive position
and tick differences. Per-class maximum speed and exact render timing remain unverified.
The existing 600 mm / 900 ms cadence remains a local Candidate policy, not an official capture
measurement. The change does not fabricate capture evidence for opcode 177.

## Verification

The C++20 Release server and a focused executable linked to the real protocol/core/world
libraries built successfully in the existing build directory. The focused C++ run passed
125 cases with zero failures; eight historical profile-comparison cases were skipped because
their separate baseline/variant inputs were not configured. These skips are not passes.
All three selected native TLS/MemoryStore tests passed: tutorial Run/prediction packets;
combat/potion action-timer regression; and an open-world private GM encounter with running
frames, Stop before attack, no spawn leaked to a second peer and no post-despawn activity.
The TLS processes used the documented child-process `OPENSSL_ia32cap=0` diagnostic; this does
not repair or certify normal OpenSSL CPU dispatch. Python checks passed: 91 combat generator
cases, 11 tutorial-spawn cases and 11 protocol-reference generator cases.

Private build/test logs and source/binary fingerprints are under
`out/npc-locomotion-fix-20261026/`. During integration,
concurrent marketplace registration lacked source/header/schema inputs; the existing protobuf
generator gained its required market request/reply roots so the registered service can build.
Generated C++ was regenerated through that tool, not hand-edited. Concurrent town-patrol,
marketplace and other world changes remain owned by their respective work; this report does
not certify those features. The strict reference check still exposes their pending reviews.

Deployment must pair the rebuilt server with the newly generated
`data/generated/tutorial/definitions.json` (whose combat block contains `npc_locomotion`).
An old profile cannot activate these GM encounters. No active service was stopped or replaced,
and no live user state was migrated or deleted.

## Remaining acceptance

No candidate original-client locomotion observation or live deployment is claimed. An already
running client was inspected without input; its GM chat identified open-world spawned NPCs as
visual-only on the old service. That is baseline evidence, not verification of this repair.
Terrain collision/navmesh routing, patrol/roaming, hearing, threat and public shared-world NPC
simulation remain incomplete. Linear server pursuit can cross obstacles and the client can
still snap explicit corrections/stops; neither behavior is represented as full NPC navigation.
