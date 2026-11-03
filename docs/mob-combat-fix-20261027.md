# Monster combat fixes — 2026-10-27

Three user-reported world-monster defects, their causes in the server and the local policies
that now apply. All behavior below is server-side, local and synthetic-tested; none of it has
original-client acceptance yet.

## Reported symptoms

1. A monster standing in front of the player plays its attack, but no damage is dealt.
2. A monster keeps chasing a player who runs away instead of returning to its spawn point.
3. A monster sometimes lands a melee hit while the player is far away.

## Causes

### Attacks that cannot land (symptom 1)

- **Centre-point hit tests.** `within_shape` tested only the target's centre point against the
  attack shape. The official collision overlaps the shape with the target's game-hit capsule,
  so a player standing at the edge of a swing, or slightly higher or lower than the monster
  (spawn heights are not terrain-projected), was missed. The player capsule in the exported
  profile is 300 mm in radius and 960 mm in half height.
- **Casting beyond the attack's own shape.** Monsters started an attack at
  `min(Skill.maxDistance, NpcTemplate.combatSuitableDistMax)`. Those catalog distances are
  often wider than the authored hit shape: in the generated world profile, 3,054 NPC damage
  events have shapes shorter than their skill's cast range (for example, NPC 5001028 cast skill
  361710200 from 3 m with a 1.25 m box).
- **Ignored attack dashes.** 394 NPC attacks have a *delayed* `AdaptedChaseTarget` root-motion
  window: the monster winds up, then dashes toward its target and hits. The server supported
  only zero-start windows (one block, 190100400), so it kept the monster where it stood while
  the attack visibly reached the player, and judged the hit from the old position.
- **Gap-closer attacks treated as ranged.** Attacks with a catalog minimum distance were
  classified as ranged and kept their full catalog range, even when their damage is a short
  caster-centred shape.

### No disengagement (symptom 2)

The leash compared only the *player's* distance from the monster's spawn point with
`Npc.returnStartDist`. That is 50 m for most placements and 100–500 m for 190 of them, and
nothing else ended an engagement while the player stayed inside that circle.

### Melee hits from far away (symptom 3)

`within_shape` had no branch for `shapeCylinder` targeting (139 NPC damage events). Those
effects fell through to the skill-range fallback, so a 1.5 m melee cylinder of a skill with a
50 m catalog range hit a player anywhere within 50 m.

## Changes

### Hit shapes (`src/world/features/combat/combat.cpp`)

- Every shape is tested against the target's hit capsule: the capsule radius widens horizontal
  bounds (including inner radii and sector bearings) and its half height widens vertical bounds.
  NPC attacks use the smallest playable-model capsule from `player_collision`; player attacks
  use the target NPC's `npc_collision` capsule. Profiles without those tables keep centre-point
  tests.
- `shapeCylinder` is a full cylinder of its catalog `radius` and `halfHeight`. It no longer
  uses the skill-range fallback.
- Unverified authored box/sphere event shapes remain unused, as the generator documents.

### Attack reach gate

- At load, each executable NPC attack gets `melee_reach_mm`: the furthest distance straight
  ahead at which any of its direct damage events still touches a standing player's capsule,
  found by running the runtime `within_shape` test, plus a supported lunge's
  `max_move_distance_mm`. Attacks with projectiles, areas or reference-target damage have no
  reach and keep their catalog range.
- Cast range, pursuit stopping distance and the first-attack engagement distance are all
  limited by that reach. A monster therefore walks into reach before swinging.
- An attack whose reach is shorter than its catalog minimum distance can never land; the
  pursuit band selection skips it instead of parking the monster inside that band.

### Delayed attack dashes (Candidate)

`supported_lunge_window` now also accepts windows that start after the cast, and treats offset
direction −180 like 180. A delayed window starts its motion when the window opens: the target
is sampled then, the monster turns toward it (`LookAtDir_ToTarget`), and the existing
`PS2C_StartX5RootMotionNoti` (64) / `PS2C_NpcMotionEndSyncNoti` (106) lifecycle is sent from that poll.
Hits after the window start are judged from the moving pose. The exact client start timing of
delayed windows is not captured, and the 64 for a delayed window has not been observed with
the original client.

### Disengagement (`src/world/features/npc/npc_ai.cpp`)

- Leash: an engaged monster returns home when the player **or the monster itself** is farther
  than `returnStartDist` from the monster's spawn point.
- Give-up timer (local policy, no official rule recovered): an engaged monster that has not
  attacked, been hit, or had its target within attack reach for 10 seconds
  (`kNpcGiveUpWithoutContactMs`) releases its target and walks home. The existing return,
  reset delay and health reset then apply unchanged.

## Validation

Isolated worktree build (`HEAD` plus only the combat and NPC AI sources), C++20 Release:

- New unit tests: `NpcAi.GivesUpAndReturnsWhenItCannotReachItsTargetWithoutAnyContact`,
  `NpcAi.AHitDuringTheChasePostponesTheGiveUp`,
  `NpcAi.AMonsterCarriedBeyondItsLeashReturnsEvenWhileItsTargetIsInside`,
  `Combat.NpcMeleeClosesToItsShapeReachBeforeSwinging`,
  `Combat.NpcMeleeCylinderNeverHitsAPlayerWhoLeftItsRadius`,
  `Combat.NpcHitShapesTouchThePlayerCapsuleNotOnlyItsCentre` and
  `Combat.ADelayedLungeWindowStartsAtItsOwnTimeAndUnmeasuredWindowsStayUnmoved`.
- Profile-gated `WorldMobs.GeneratedMonstersLandTheirAttacksOnAStandingPlayer` provokes every
  generated monster type with locomotion and an attack against a player standing 4 m in front
  of it for 10 s. Result on the current generated profile: 838 types simulated, 0 attacking
  without ever landing a hit, 0 never attacking. The same simulation on an unmodified `HEAD`
  build: 152 of 838 types attacked for 10 s without ever landing a hit.
- Full `chronicles-tests` run in the isolated build: 868 passed, 72 optional-environment skips.
  The single failure, `Combat.AreaSpawnHitLandsAtTheCreatedTickFromAThirtyTwoBitAreaUnit`,
  belongs to concurrent area-spawn work in the shared tree and exercises no changed path.
  With the generated tutorial profile and catalog, six native TLS tests (two in-place revive
  fixtures, gathering snapshot, fall damage and drowning) fail identically on unmodified `HEAD`.
- Updated tests whose negative controls asserted the old "swing from out of reach and miss"
  behavior: the lunge suite (`StandingTwoMetre...`, `TheLungeHitIsEvaluated...`,
  `ALungeWindowWithAnyUnimplementedSetting...`), `NpcEventShapeRespectsItsInnerAndOuterRadius`
  and the ranged-selection fixture (its ranged skill now has a real ranged effect).

These are synthetic server checks. The original client's rendering of delayed dashes, the
10-second give-up and the corrected hit outcomes have not been observed.

## Remaining limits

- Attack dashes are still straight lines without collision or navmesh; only the
  `AdaptedChaseTarget` back-off semantics are simulated.
- Attacks whose movement lives only in their animation (no exported root-motion block, for
  example 100100100 or 204100200 with a catalog minimum distance) cannot be simulated and are
  not selected when their shape cannot reach their minimum distance.
- Official leash, threat, chase-timeout and reset rules remain unrecovered.
