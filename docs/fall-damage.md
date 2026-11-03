# Player falls and landing damage

`src/world/features/combat/fall_damage.*` tracks each player's accepted movement on the world-session worker
strand. A Falling sequence records its highest Z, including the preceding grounded
position. The first Normal or Crouching report ends that fall and applies damage once.
Normal movement alone never accumulates drop distance, so walking down slopes does not
cause damage. Negative heights are valid; coordinate subtraction uses 64-bit arithmetic.

Swimming, gliding, flying, climbing, parkour and root motion end the tracked fall without
impact. Map loads, server map moves, completed teleports, death and revival discard old
fall state. These exclusions are conservative local policy; they do not establish the
official game's complete movement or landing-skill semantics.

## Damage policy and evidence

The matching exported GameVariable table contains `FallHeightSafe=1000`. This implementation
interprets that value as engine centimeters and converts it to 10,000 wire millimeters.
The conversion is **Candidate**, consistent with the existing movement conversion research
in `docs/movement-protocol.md`; the fall-variable consumer itself has not been recovered.
Missing or invalid catalog values retain the documented 10 m default.

The damage curve is an explicit **local gameplay policy**, not a recovered official formula:

- Falls up to the safe height cause no damage.
- Above the safe height, each extra meter costs 5% of maximum HP, rounded upward.
- At 20 m above the safe height, damage reaches 100% of maximum HP.
- With the current catalog, 15 m costs 25%, 20 m costs 50%, and 30 m is lethal.

Impact bypasses attack and armor calculations. Maximum HP sets the damage amount; remaining
HP is clamped at zero. Repeated ground reports and reports from a dead player cannot charge
the same fall again. No client damage amount, velocity or timestamp determines the damage.

## Health, death and persistence

Tutorial combat and the fall system share `CombatSession` player vitals. Sessions outside
the tutorial combat map initialize the same vitals without private NPC simulation, provided
the server has loaded the verified character-stat and combat data. This covers supported
follow-up and shared-map entry and server map transitions.

The owner receives DamageNoti (19) with a health StatChange. A lethal impact follows the
existing player-death order: Demise (32), damage (19), death (23), regen checkpoint (54),
then stat/status cleanup (47 and applicable removals). Existing revision-guarded death
persistence and equipment durability loss complete before those packets are encoded.
Shared-map death also persists before publication. Reconnect reloads the dead flag;
the existing in-place revive request clears it before restoring health.

The environmental damage/death presentation uses default caster and skill fields. The
recovered schemas accept this shape; official fall attribution and original-client
rendering have not been observed. This change reuses the ordinary combat-death cleanup;
it does not claim new official evidence for that packet ordering.

## Limits and validation

Positions and movement modes remain client-reported. There is no server terrain collision,
ballistic simulation, movement-speed authority or proof of ground contact. A client that
omits Falling or falsely reports a supported movement mode can evade this bounded system.
Disconnecting during a fall discards its transient state. Root-motion landings and mounted
or vehicle impacts are not reconstructed. Observer-side shared-world damage presentation
and fall-specific invulnerability rules remain unverified. Living HP retains the existing
session-only persistence contract; the dead/alive transition is durable.

`FallDamage.*` tests cover thresholds, scaling, numeric bounds, ledge/apex tracking,
one-shot landings, movement exclusions, map/reset isolation, health notification encoding,
death cleanup and revival. Native TLS tests exercise safe, damaging and lethal falls,
death persistence, reconnect and revival in tutorial and shared-map sessions. Test source
is separate from execution evidence; actual results are recorded in the feature reference.

No original-client fall trial or live server deployment is asserted by this document.

Execution on 2026-10-26: the isolated Release build passed all 14 focused cases
(12 unit/schema/handshake checks and two TLS/MemoryStore tests). Both existing native
revival regressions passed with sufficient time for their fights. The broader combat
run passed 84 cases, skipped 13 environment-gated cases, and failed six NPC movement
cases. Those same six cases fail with the combat fall additions removed from the frozen
integration snapshot; they are not certified fixed here. The test processes used the
existing documented portable OpenSSL CPU-dispatch diagnostic. No PostgreSQL trial ran.
