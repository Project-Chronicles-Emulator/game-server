# Item effects: damage, stat multipliers, ailments, areas and ItemUseReq

Scope: the server rules behind the item effects that [item-use.md](item-use.md) lists as
consumed without effect before 2026-10-26, and the evidence for each. Labels: **Observed** =
decoded official capture, **Static** = client catalog or client code, **Candidate** = a guess
or a fit that is not proven, **Synthetic** = emulator source tests only.

## Damage (Candidate fit to Observed hits)

The local model (fixed damage + coefficient x PhysicalPower/MagicPower) ignored attack power and
armor, so attack and armor buffs had no effect. Both directions are now fitted to the four
capture sets. Each hit was paired with the attacker's and target's stats at that moment (the
player's from MapMoveCompleteNoti and ChangeStatNoti, an NPC's from its UnitAddNoti).

**Player on NPC** (867 hits; every residual within 1.4 points; 882 of 882 within 1 point after
rounding, fixed-damage hits included):

```
damage = 5.7685 x (physicalCoefficient x PhysicalAttackPower + magicCoefficient x MagicAttackPower)
         + 47.109 - 0.5717 x armor
```

`armor` is the target's physical and magic armor weighted by the two attack shares. The
coefficient term's scale is identical (5.763 to 5.785) across target armor 11 to 37, so armor
lowers the hit by a constant amount rather than by a percentage. PhysicalPower (stat 22) does
not affect the result. Fixed damage (the throwing dagger's 2120) lands unchanged.

**NPC on player** (89 hits; 91% exact after rounding, 83 of 87 within 2 points):

```
damage = 1.2106 x coefficient x attack x 44.88 / (44.88 + armor)       per physical and magic share
       + 1.0282 x elemental coefficient x elemental attack x 44.88 / (44.88 + elemental armor)
```

Candidate parts: the elemental term for player skills (no captured player effect has one), the
Incoming/Outgoing damage stats read as per mille, armor clamped at the catalog minimum of 0, and
the player constant term applying only to a hit that has an attack share. Critical hits, guard,
shield and dodge are not reconstructed. Fixtures whose caster stats lack attack ratings
(stats 53/48) keep the old local model.

## Status stat multipliers (Static display rule, Candidate application)

`MaintainStat.statMul` is applied as a percentage in 1/1000 percent units: the client tooltip
`AAC_PerkNode_FormatStatDescription` (0x146253490) shows a multiplier as
`floor(value x 0.01) x 0.1` percent (IDA record `dumps/ida_research/item-effects-20261026`). So
10000 is +10% and 200 is +0.2%. The server adds `statAdd` first and applies the multiplier to
the result (order Candidate); the change is stored with the status and reverted exactly at
removal. 90 MaintainStat rows carry a multiplier.

Statuses now apply to monsters too (debuffs the player places and ailment statuses), with the
same add/update/remove packets (49/50/51) naming the monster unit. Monster stat changes are not
reported (the client shows no NPC stats).

## Ailment gauges (Static rules, Observed decay, Candidate gain)

GameVariable rows define, per abnormal type, the gauge maximum (20000), the decay per 100 ms
(Burning 40, Freezing 32, ElectricShock 60, Bleeding 48, Poisoning 28, Stun 60, Shrink 60) and
the status a full gauge applies (12 to 18). Observed: `PS2C_ChangeAbnormalStatusGaugeNoti` (52)
carries the value at its update tick and the client decays it from there (the captured Bleeding
10256 had decayed to 9632 after 1341 ms, exactly 13 x 48); a filled gauge is reported empty,
without a value, before its ailment status.

- A damage effect's `abnormalStatus<Type>Attack` raises the target's gauge by 1.7 x the attack
  value. **Candidate:** eight official gains measured 1.51 to 2.50 x the catalog attack, with no
  caster power, resist, level or damage stat explaining the spread; 1.7 is their median.
- `addAbnormalGauge` effects change a gauge directly: cures lower the user's gauge (for example
  `burningAdd -20000`), and the trained dagger's chained `bleedingAdd 7000` raises its target's.
- Immunity potions apply statuses whose `immune` field names an `Immune` row (27 to 32); while
  such a status lasts, gains of those types are ignored.
- Ailment damage over time (tickSkillEffectId 0 in this catalog) is not simulated.

## Areas of effect (Static data, Observed packet shapes)

The balm data exists in the client: `spawnAreaOfEffect.areaOfEffectId` names a `SkillObject`
row directly (the separate `AreaOfEffectSpawner` table holds one unrelated level trap). For
example SkillObject 100556 is an `AOEIntervalHit` cylinder (radius 300 cm, half height 100 cm,
20 s) that runs effect 100800 every 1000 ms; 100800's `FK_statusEffectId` 100557 is the debuff
(PhysicalArmor -100 for 20 s). The generator now exports 79 such areas, each interval effect,
the effects' `FK_statusEffectId` and projectiles' hit-despawn effects (the poison bottle's
cloud, SkillObject 100302, spawned where the bottle lands).

Observed packet shapes (an official poison-bottle cloud): `PS2C_UnitAddNoti` with a
`skill_object` nub (`type AreaOfEffect`, `skillobject_cms_id`, caster, created tick,
duration); interval hits name the area unit as caster, the thrower as base caster and the
`AreaOfEffect` origin; `PS2C_DespawnSkillObjectNoti` (45, LifeTime) at the end of the lifetime;
`PS2C_UnitRemoveNoti` 1496 ms later. Area unit ids use type nibble 3, as every captured area id.

Candidate: the first hit lands at spawn; a conditional interval effect uses its
ConditionalSkillEffect default branch; the area hits the caster's enemies inside its cylinder
up to the effect's target limit, regardless of the effect's Operable unit-type filter; a
`Stacking` status (the balm debuff) is refreshed instead of stacked.

## PC2S_ItemUseReq (Candidate)

No ItemUseReq was captured. Consumables without an item skill are served through it:

- Draw boxes (80 items, `Item.FK_drawBoxId`): one use spends one box and grants its `DrawBox`
  rows. Candidate draw rule: rows without `drawWeight` are always granted; one row is picked
  from the weighted rows proportionally to their weights; quantities are uniform in
  [minAmount, maxAmount]. Gold and Hersander Tokens, stacks, housing objects and equipment
  instances (in free inventory slots) are granted in one storage commit.
- Effect consumables (`Item.FK_skillEffectList`, the four FGT food buffs): one use spends the
  item, then applies the listed effects to the user through the combat session. They need the
  combat session, so they are usable only in the private tutorial instance.
- Summon registrations (ten horse items) stay with the companion session, which already handled
  them through ItemUseReq.

The reply is `PS2C_ItemsUpdateNoti` with the `use` reason (as the companion registration path
sends); a refusal is the common error response (opcode 2) and changes nothing.

## Verification

- C++: `CombatRules.*` (official hit reproduction, multiplier unit, gauge decay),
  `CombatItemEffects.*` (cure and immunity, gauge overflow, balm area lifecycle and armor
  debuff, monster gauge gain, food multiplier) and `ItemUse.*` (draw rules, storage kinds,
  refusals, effect consumables), all Synthetic.
- Python: `tools/test_build_combat_data.py` exports for areas, immunities, gauge rules, effect
  lists and the effects' status column.
- Not observed with the original client against the emulator; ItemUseReq routing through the
  world frame path has no network test.
