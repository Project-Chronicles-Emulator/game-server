# Combat effect-kind contract (issue #63)

Scope: which catalog effect kinds the generated combat profile carries, which of them
`CombatSession::apply_effect` handles at `eb0834b9`, and the catalog schema of the top
unhandled kinds. Generator-side only; no runtime change is proposed here.

Labels: **Static** = catalog/generated profile, **Observed** = decoded official capture,
**Candidate** = not proven.

## Effect kinds referenced by skills (Static)

Profile built from the committed tutorial observations (704 skills: 674 with a player class,
30 tutorial-NPC skills; no skill is both). Counts are *distinct skills whose stage events
reference an effect of that kind*, so one skill can count under several kinds.

| Kind | Player-class skills | Tutorial-NPC skills | Handled at eb0834b9 |
| --- | ---: | ---: | --- |
| damage | 492 | 38 | yes, `combat.cpp:586` |
| spawnAreaOfEffect | 147 | 0 | **no** |
| addStatusEffect | 130 | 0 | **no** |
| spawnProjectile | 127 | 3 | yes, `combat.cpp:644` |
| changeWeaponSkillSlot | 65 | 0 | **no** |
| removeStatusEffect | 55 | 0 | **no** |
| addShield | 6 | 0 | **no** |
| restoreHP | 5 | 0 | yes, `combat.cpp:678` (player caster and target only) |
| attachAura | 2 | 0 | **no** |
| skillCooldownReset, addAbnormalGauge, dispelShield | 1 each | 0 | **no** |
| statRecovery | 1 | 0 | yes, `combat.cpp:683` (player caster and target only) |

Closure of the exported `effects` table (chains and projectile hit effects included, 954 rows
in the open-world profile): the same kinds plus `heal` (1, handled with restoreHP at `:678`) and
`grab` (1, unhandled). Every effect id a skill references resolves to an exported row.

Unhandled kinds fall through `apply_effect` silently: no packet, no state change. The `chain`
effect is still followed (`combat.cpp:696-699`, depth limit 8). Handling is a runtime decision
for the integrator; this document only fixes the data contract.

Update for item skills (see [item-use.md](item-use.md)): the profile now also exports the
Item-type skills that spend an item, with their catalog events. `useItem` spends the item,
`statRecovery` announces the official recovery flow (54, 80), self-cast `removeStatusEffect`
removes the player's statuses of that id, and self-cast `addStatusEffect` applies the
status's `MaintainStat.statAdd` modifiers. Since the item-effect rules
([item-effects.md](item-effects.md)), `addAbnormalGauge` changes ailment gauges,
`spawnAreaOfEffect` spawns the named SkillObject area, a player's `addStatusEffect` on a monster
applies the status there, and a damage effect's `status_effect_id` (SkillEffect
`FK_statusEffectId`) attaches its status to the target. Line numbers above refer to `eb0834b9`
only.

## addStatusEffect (Static, 70 effect rows in the tutorial closure)

`parameters` keys, all JSON numbers; the catalog writes some as `7000.0` and some as `7000`,
so consumers must accept both and convert with an integral check:

| Key | Present | Observed values | Meaning |
| --- | ---: | --- | --- |
| statusEffectId | 70/70 | 70/70 resolve to a `StatusEffect` row | Proven: references an exported row |
| stackCount | 70/70 | 0, 1, 2, 3 | Candidate: stacks added; 0 is not explained |
| rate | 70/70 | 100.0 only | Candidate: percent chance; only 100 is exercised |
| level | 66/70 | 1 only | Candidate |

Targeting is `refTarget` for 69 rows and `shapeCylinderSector` for 1; 4 rows have a chain.
The generator enforces only the referential part (`validate_status_effect_references`), which
the catalog proves. It does not enforce meanings of `rate`, `stackCount` or `level`.

## removeStatusEffect (Static, 28 rows)

Keys: `statusEffectId` (23/28), `rate` (28/28, 100.0 except one int), `deleteCount` (28/28),
`stackCount` (25/28), `groupId` (6/28). Rows without `statusEffectId` remove by `groupId`.
Where `statusEffectId` is present it resolves to an exported row. `fragment()` in the generator
already accepts the three catalog rows that use bare property names.

## spawnAreaOfEffect and changeWeaponSkillSlot (Static)

- spawnAreaOfEffect: 69 rows, one key `areaOfEffectId` (float), targeting `refTarget`. The
  `AreaOfEffectSpawner` table is **not** exported into the profile; consuming this kind needs
  that join and is out of scope here (Candidate).
- changeWeaponSkillSlot: 54 rows with empty `parameters`; the effect is addressed by the
  `ChangeWeaponSkillSlot` table, also not exported (Candidate).

## StatusEffect rows (Static) and units (Observed)

Generated `status_effects` carries 507 rows. Relevant fields: `duration_ms`, `stack_rule`,
`tick_ms`, `maintain_effect_ids`.

- Units: catalog `duration` is milliseconds. Observed `expire_tick - start_tick` in
  `PS2C_AddStatusEffectNoti` is nanoseconds; dividing by 1,000,000 gives milliseconds that equal
  the catalog value (details and table in `combat-status-effect-evidence.md`).
- **Empty `duration`**: 5 of 507 rows have an empty catalog duration; the generator emits
  `duration_ms = 0` for them. Two of those rows were observed with
  `expire_tick = 9223372036854775807` (int64 max, treated as no expiry), one was observed with
  300000 ms. So `duration_ms == 0` does **not** reliably mean "permanent"; a runtime must not
  infer expiry behaviour from it (Candidate).
- Stack rules among the 66 distinct rows referenced by addStatusEffect effects: `ResetDuration` 55, `Stacking` 11
  (`stackRuleValue1` 0 to 10). `tick` is 0 for all 66 and `jsonCase` is empty for all 66.
- `ui_type`: Buff 24, Debuff 22, Hidden 20.

## Provenance

- Catalog: `data/generated/catalog/Tables/{SkillEffect,StatusEffect}.jsonl`, read through the
  hash-checked `Source` class used by `tools/build_tutorial_data.py`.
- Handling lines: `git show eb0834b9:src/world/features/combat/combat.cpp`.
- Observations: three 2026-10-24 capture sets listed in `docs/capture-corpus.md`; only
  aggregates are recorded.
