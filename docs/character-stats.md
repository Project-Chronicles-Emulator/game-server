# Tutorial character info and stats

`CharacterStats` loads immutable, hash-checked client definitions at startup for
build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`, executable dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
It reads CharacterBasicStat, Item, ItemAttribute, EquipmentGrade, Stat limits,
and durability GameVariables. No catalog/file work occurs in socket handlers.

The service constructs initial stat maps for Fight, Wild, Adamant, and Magic.
World entry sends local character metadata through WorldCharBaseNoti (125),
followed by inventory; MapMoveCompleteNoti and nearby-player snapshots use the
same prepared stats. Names, identity, level, and EXP come from local storage.

## Supported initial values

| Class | HP | SP | Physical power | Magic power | Physical armor | Magic armor | Physical amplification | Magic amplification | Critical rating |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fight | 309 | 540 | 190 | 76 | 45 | 30 | 35 | 9 | 22 |
| Wild | 248 | 567 | 117 | 99 | 57 | 41 | 17 | 14 | 37 |
| Adamant | 363 | 651 | 114 | 30 | 84 | 68 | 28 | 6 | 9 |
| Magic | 234 | 540 | 77 | 197 | 33 | 45 | 5 | 37 | 23 |

These examples use the captured starter weapons/armor, plus the captured initial
necklace for Wild/Adamant. Equipment values are calculated from the character's
actual equipped instances, not copied into class constants. The necklace adds
one physical armor point. Unequipping it removes that point.

Core power uses the recovered piecewise STR/INT curve plus Dex. HP and SP now use
the recovered nonlinear CON/END curves plus starting baselines 200/500. Armor is
`ceil(2.1*End + .35*Tal)`. Core amplification is
`ceil(log(.0307*Power+1.5)^5*.6248528243076684+.1)`.

Equipment attack amplification uses
`log((level+20)^2/300)*52.26579203455843+1`; armor uses
`log((level+26)^2/700)*90.89702962531902+9`. Multiply by attribute and grade rates,
apply the durability multiplier, and round each item's contribution before
adding it to core stats. Attribute export rates divide by 100; grade rates divide
by 1000. Basic-stat bonuses have a separate recovered float/logarithm curve.
Only equipped items contribute. The active weapon is selected explicitly;
Weapon_1 is the bootstrap default, and both weapon slots are never summed.

At or below the catalog danger ratio (.1), durability reduces applicable
equipment contributions by the catalog penalty (.5). Hit/impact omit this
penalty and the level curve. Stat limits are loaded from the catalog and applied
after contributions. Invalid classes, unknown equipment/grades, duplicate or
wrong slots, invalid durability/quantity, and malformed definitions fail clearly.
Fixed hit/impact armor is supported for integral values. The client's item detail
retains one decimal for fractional values; their server wire rounding remains
unresolved, so the candidate rejects that case explicitly.

## Critical and elemental fields

Physical/magic critical ratings are `ceil(1.35*Dex)`; resistance uses `.7*Con`,
with physical nearest rounding and magic ceiling. Penetration is
`ceil(.1*Str+.6*Dex)` for physical and `ceil(.1*Int+.6*Tal)` for magic.

Send raw critical ratings and separate bonus keys 82/83/89/90. The client converts
ratings into displayed chance and critical damage; sending a percentage as the
rating is incorrect. IDA verifies the chance clamp 200..3000 and damage clamp
1000..5000, with minimum displays 2% and 110%. This research does not establish
combat damage mitigation or the damage interpretation of amplification ratings.

All six elemental attack ratings use
`ceil(log(coefficient*Tal+1)^4*.27075605219327015)`. The coefficients are .09 for
Electric/Iron, .15 for Fire, .14 for Fog, .11 for Tidal, and .2 for Wind. Assembly
verifies the distinct branch inputs, which were omitted in the old cached
pseudocode. These formulas reproduce the captured Tal21 Fire/Wind and Tal22 Fog
thresholds and handle equipment Talent bonuses. Starting ratings are all one,
except Wind is two for Wild/Magic. Unmodified elemental armor/penetration and
critical bonuses are zero. Equipped elemental rates use the same verified
equipment curve families.

## Boundaries and validation

API: `CharacterStats::load(ClientCatalog&)`, `stats(Character)`, and
`stats(Character, active_weapon_slot)` (0 or 1). The original one-argument public
signature is preserved. `ClientCatalog::rows` returns a copy under the catalog
lock for startup indexing; definition state is immutable after loading.

Current HP/SP start full; combat owns depletion and recovery afterward. Do not
recompute bootstrap stats to reset an active combat unit. Allocated LevelUpPoint
attribute bonuses (`Character::build.stat_bonuses`) are added to the class base
attributes before every derived formula, and earned `Stat` perk nodes add their
flat `statAdd`; see [progression.md](progression.md). Reconnect durability,
permanent RewardStat grants, buffs, options/sockets and durable current vitals
require their own state integration.

Item tempering (`polish_step`) is applied as the client applies it: every
curve-based attack, armor and elemental term on any slot, and every basic-stat
bonus, is multiplied by `1 + ItemPolishIncreaseValue percent / 100`; fixed
hit/impact terms are not, and a step outside the item's increase table adds
nothing. Only the equipped-weapon attack frames are captured; the rest follows
the static client formulas (see docs/inventory-polish-evidence.md). This change
does not persist an active-weapon choice.

Nine character-stat checks cover baseline vitals, captured four-class advanced
ratings, active weapons, durability boundaries, elemental equipment and grade
scaling, distinct Talent thresholds, invalid equipment, protocol bootstrap, and
optional private capture comparison. The applied shared Release builds for tests
and server passed on 2026-10-24, followed by all 50 focused stats, catalog, NPC,
combat, and NativeWorld checks with real catalog/tutorial profiles. Test processes
used the coordinated process-local portable OpenSSL fallback. Both optional C++
capture checks passed. These checks remain separate from original-client visual
verification. Read-only inspection subsequently confirmed tutorial world entry,
but Character Info was not open and no stat values were visible; the live panel
comparison remains pending. The earlier realm-selection blockage is stale.

The preserved local Wild character currently has legacy cloth armor and no
necklace. Its calculated physical/magic armor is 41/53, physical/magic power is
117/99, amplification is 17/14, critical rating is 37, and maximum HP/SP is
248/567. The captured Wild example above uses different equipped items; those
values must not replace calculations from persisted equipment.

The independent verifier checks hash-validated catalog input and reports numeric
discrepancies without player/session identifiers. For unallocated level 1â€“2,
single-weapon snapshots, it reports 99 matching comparisons in the older capture
and 165 in the full tutorial capture. It excludes two-weapon snapshots because
their nubs do not identify active selection. Later snapshots can contain rewards,
perks, polish, options and other unmodeled modifiers, and may report mismatches.
The command returns nonzero when comparisons disagree. Across the broader
single-weapon snapshot set, all 294 elemental comparisons match, including
modified Talent values.

```powershell
python tools/verify_character_stats.py <decoded.jsonl> --catalog data/generated/catalog --initial-only
```

IDA research, cached pseudocode, fingerprints, and detailed formulas are under
`dumps/ida_research/character-stat-formulas-20261024/` in the parent workspace.
56 functions were annotated and the matching `.i64` database was saved with
stored-section verification. The global function index and append-only journal
preserve prior records.
