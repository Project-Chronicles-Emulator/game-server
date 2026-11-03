# Item use (consumables with an item skill)

Scope: what the server does when the client uses an item through
`PC2S_StartSkillReq{start_skill, use_item}` (opcode 20010), which items that covers, the
official evidence behind each packet, and what is still missing.

Labels: **Static** = client catalog or generated profile, **Observed** = decoded official
capture, **Candidate** = not proven, **Synthetic** = emulator source tests only.

## Request path (Observed)

Every official item use in the four 2026-10-24/26 capture sets (`docs/capture-corpus.md`) is a
`StartSkillReq` that names the item's catalog skill (`Item.FK_skillId`) and carries
`use_item{item_cms_id}` with no `item_id`. `PC2S_ItemUseReq` (20077) was never sent.

| Skill | Item kind | Requests | Started | Stack update seen |
| --- | --- | ---: | ---: | ---: |
| 1000 | Health potion 40101101 | 22 | 22 | 22 |
| 1001 | Throwing dagger 15010401 | 25 | 20 | 20 |
| 917 | Stamina potion 40301201 | 4 | 4 | 3 |
| 900 | Ice wedge 15010405 | 2 | 2 | 2 |
| 1003 | Poison bottle 15010403 | 1 | 1 | 1 |

The stack update is `PS2C_ItemsUpdateNoti` (169) with `stackables[{item_cms_id, quantity}]`
and the empty `utility_bag` reason, sent at the action's `useItem` event (435 ms for the
potion, 0 to 24 ms for throwables, 1343 ms for the stamina potion). No `UtilityBagSetRes`
(104) follows a consumption.

## Server behavior (Synthetic, tutorial and shared fields)

The combat session owns item use whenever its profile carries `item_skills` and the world
session connects the current committed stack inventory (`CombatSession::set_item_inventory`).
Tutorial and shared-field sessions resolve their respective inventory owner dynamically.
Fields without an NPC population also run item timers. Profiles without item-skill data retain
the older tutorial-only fallback; that fallback consumes the potion but has no effect.

1. **Start.** The item must map to exactly the requested Item-type skill
   (`Skill_UnusableItem`), carry no item instance id (`Skill_InvalidUseItem`) and be owned
   (`Skill_NotFoundUseItem`). An Item-type skill without `use_item` is refused
   (`Skill_InvalidUseItem`), so item effects cannot be cast for free. Catalog cooldowns
   (for example 4 s for elixirs, 30 s for immunity potions) and cancel windows apply as for
   every other skill. Accepted use replies with 53 and 14. Empty quantity instead returns
   13 with the skill/instance identifiers and no animation-start packet. Static client
   recovery shows that this refusal resets the predicted action; network latency can still
   permit predicted frames before the refusal reaches the client.
2. **Consumption.** At the `useItem` event the item is spent once per cast through
   the current inventory owner, which commits the decremented stack (and the first
   utility slot holding it) under the progress revision before the 169 is queued. Failed
   persistence does not mark the item spent. If the item
   left the inventory during the animation, the action ends (15) and none of its effects apply.
3. **Effects.** The remaining events and chained effects run through the normal combat effect
   engine, using the catalog skill timing.

## Effects by kind

Counts are the 124 catalog items (83 skills) whose exported skill spends the item (Static).

| Effect kind | Items | Behavior | Evidence |
| --- | ---: | --- | --- |
| `statRecovery` (health potions) | 11 | `StatSyncNoti` (54) flow then `AddStatRecoveryNoti` (80); the server adds the same gain silently, proportional to elapsed time, capped at the maximum | Observed 22 of 22 potion uses: 169, 54, 80 in that order, rate 200/s = 20 per 100 ms, 1 s |
| `addStatusEffect` (elixirs, crystals, stamina potion) | 30 (+6) | 47 (`SCS_StatusEffect`) with the catalog `MaintainStat.statAdd` modifiers, then 49; at expiry 47 then 51; a `ResetDuration` status already active is refreshed with 50 | Observed status 100597: 3 of 3 adds and 3 of 4 removals preceded by the 47 (MaxSP, then SP); 2001005 refresh kept its instance |
| `spawnProjectile` (daggers, orbs, bottles, javelins) | 51 | Combat projectile path; hit effects deal the catalog damage, and a flight's hit-despawn effect runs where it lands (the poison bottle's cloud) | Observed dagger damage 2120 |
| `addAbnormalGauge` (cures) | 10 (+6) | Lower the user's ailment gauge (`ChangeAbnormalStatusGaugeNoti`, 52); immunity statuses block new build-up | Observed gauge decay; cure itself Candidate |
| `spawnAreaOfEffect` (armor-reduction balms) | 16 | Spawn the SkillObject area at the user; every second it debuffs up to five enemies inside it (for example PhysicalArmor -100 for 20 s) | Static data, Observed area packet shapes |

The six items whose skills never spend them (aiming modes 997/1025/1026, party skill 1017 and
test skill 1005) are refused instead of granting an endlessly reusable animation.

`PotionHealRate` (stat 91, per mille, neutral 1000) scales recoveries whose catalog row sets
`usePotionHealRate`. Status `statMul` multipliers apply in 1/1000 percent, and the fitted damage
formulas read attack power and armor, so attack and armor buffs and debuffs change damage. The
rules and their evidence are in [item-effects.md](item-effects.md).

**Candidate:** a recovery whose catalog `cancelType` is `EndSkillStage` stops when the
casting action ends early (for example a cancel by a dodge). No official cancelled recovery was
captured; the stop is sent as the same flow instance without a rate, following the observed end
of a sprint flow.

## Not implemented

- Unsupported item kinds and profiles without recovered item skills/effects remain partial.
  Draw boxes (`PC2S_ItemUseReq`, see [item-effects.md](item-effects.md)) retain their existing
  quantity validation and atomic reward path.
- Ailment damage over time, `Stacking` stack counts, move-cancel of an item animation and the
  official server's use of the client effect tick as the flow start.

## Verification

- Python: `tools/test_build_combat_data.py` (item skill map, catalog events for item skills,
  status stat modifiers, recovery fields, real-profile export of skill 1000 and status 100597).
- C++: `src/world/features/combat/tests/combat_item_tests.cpp` (`CombatItemUse.*`) and the profile-gated
  `NativeWorld.TutorialTlsPickupPersistsAndCharactersRemainInstanced`, which drinks the
  generated potion through the real TLS frame path and receives 169, 54, 80 then 15.
- `NativeWorld.SharedFieldPotionConsumesLastItemAndRefusesEmptyAnimation` verifies committed
  zero quantity, cleared quick-slot reservation, and no 53/14 on the repeated use through TLS.
  Inventory owner tests cover a failed persistence commit without inventory mutation.
- Not observed with the original client against the emulator.
