# Wild skill cost and cooldown evidence

**Observed** in three private 2026-10-24 world captures: 4,077 / 36,676 /
30,219 complete records, respectively; native-log build ID
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa` (not the executable SHA-256).
Binary capture SHA-256 values are in `docs/capture-corpus.md`. Only skill IDs,
packet opcodes, result names, and relative durations are published here.
Player/session IDs, positions, and payloads remain private.

## Matching method and limits

**Observed:** A bounded pass over all three `decoded-analysis.jsonl` files
matched `PC2S_StartSkillReq` **20010** by `(skill_id, instance_id)` to a
subsequent `PS2C_StartSkillNoti` **14** for the current MapMoveComplete player
unit or `PS2C_StartSkillFailRes` **13**, within 2,000 ms. The current unit was
obtained internally from `MapMoveCompleteNoti` **176** and is not printed. A
`SkillUtilizationCostNoti` **53** was joined by unique outstanding
`skill_instance_id` within the same interval. The accepted gap is the
minimum elapsed capture milliseconds between two accepted notifications for
the same skill **within a capture**, not a measured enforced cooldown.

**Observed:** 2,236 requests; 2,214 matched accepted notifications and
21 matched failures. One skill-1420 request has no classified matching
outcome under the bounded join and must not be declared accepted.
`53.stat_changes` records `stat_type=72` and negative deltas; these match
catalog `Skill.costs` `SP:<amount>` on every accepted skill with a recorded
SP cost. The stat-type name is inferred from this agreement, not from a
numeric enum proof. The `53.cooldowns[].expire_tick - tick` interval,
divided by 1,000,000 (native nanosecond ticks per ms), matches catalog
`Skill.cooldown` for observed nonzero deadlines. Other `StatSyncNoti` **54**
and `UnitChangeAttributeNoti` **33** may follow the request; their proximity
alone does not attribute a cost to a skill. No mana delta was established.

**Static:** catalog `data/generated/catalog/Tables/Skill.jsonl`, fields
`id`, `weaponType`, `skillType`, `costs`, `cooldown`, `cooldownGroup`.
An empty cost is not an observed resource deduction. `cooldown=0` is a
catalog value, not proof that the client can repeat instantly. No combat
hotbar is assumed; prompts are contextual. Item skills are common
(`weaponType=None`) and are not weapon perk unlocks.

## Per-skill requests, outcomes and catalog comparison

All rows are **Observed** for request/outcome/cost and gap columns and
**Static** for catalog columns. `—` means no measured value, not zero.
`0` under cooldown is an explicit catalog/deadline value. Cost format is
catalog / observed 53 stat-72 delta. Deadline format is catalog / observed
53 minimum milliseconds. A zero deadline can accompany a large repeat gap
for ordinary gameplay reasons.

| Skill | Weapon; type | Req → accepted | Catalog SP / 53 delta | Cooldown catalog / 53 ms | Min accepted gap ms | Matched failures (13.ec) |
| ---: | --- | ---: | --- | ---: | ---: | --- |
| 900 | None; Item | 2 → 2 | — / — | 0 / 0 | 2834 | — |
| 917 | None; Item | 4 → 4 | — / — | 4000 / 4000 | 32158 | — |
| 1000 | None; Item | 20 → 20 | — / — | 0 / 0 | 2366 | — |
| 1001 | None; Item | 23 → 18 | — / — | 0 / 0 | 421 | Skill_UnusableUtilityBag×5 |
| 1003 | None; Item | 1 → 1 | — / — | 0 / 0 | — | — |
| 1327 | LongBow; SwapWeapon | 4 → 4 | — / — | 0 / 0 | 259143 | — |
| 1401 | LongBow; SheathWeapon | 138 → 138 | — / — | 0 / 0 | 1097 | — |
| 1409 | LongBow; Dodge | 210 → 207 | SP:50 / -50 | 0 / 0 | 403 | Skill_InReaction×3 |
| 1416 | LongBow; Normal | 17 → 17 | SP:50 / -50 | 0 / 0 | 707 | — |
| 1417 | LongBow; Normal | 6 → 6 | — / — | 0 / 0 | 54098 | — |
| 1418 | LongBow; Normal | 586 → 576 | SP:10 / -10 | 0 / 0 | 353 | Skill_Condition_NotEnoughCost×9, Skill_InReaction×1 |
| 1420 | LongBow; Normal | 387 → 386 | SP:15 / -15 | 0 / 0 | 903 | — |
| 1421 | LongBow; Normal | 145 → 145 | SP:70 / -70 | 0 / 0 | 1874 | — |
| 1422 | LongBow; Command | 126 → 126 | SP:45 / -45 | 0 / 0 | 1162 | — |
| 1423 | LongBow; Command | 54 → 53 | SP:55 / -55 | 0 / 0 | 1789 | Skill_UnusableSkill×1 |
| 1424 | LongBow; Normal | 3 → 3 | SP:65 / -65 | 0 / 0 | 178503 | — |
| 1427 | LongBow; Normal | 26 → 26 | SP:100 / -100 | 0 / 0 | 4000 | — |
| 1433 | LongBow; Dodge | 7 → 7 | SP:70 / -70 | 0 / 0 | 48253 | — |
| 1434 | LongBow; Normal | 23 → 23 | SP:20 / -20 | 0 / 0 | 517 | — |
| 1445 | LongBow; Normal | 45 → 44 | — / — | 0 / 0 | 589 | Skill_InReaction×1 |
| 1446 | LongBow; Normal | 24 → 24 | — / — | 0 / 0 | 1584 | — |
| 1452 | LongBow; Dodge | 40 → 40 | SP:80 / -80 | 0 / 0 | 1300 | — |
| 1461 | LongBow; LandingSkill | 6 → 6 | SP:50 / -50 | 0 / 0 | 1540 | — |
| 1462 | LongBow; Normal | 3 → 3 | SP:110 / -110 | 0 / 0 | 17563 | — |
| 1463 | LongBow; Command | 67 → 67 | SP:45 / -45 | 0 / 0 | 586 | — |
| 1100000 | HandAxe; Normal | 29 → 29 | SP:15 / -15 | 0 / 0 | 552 | — |
| 1100002 | HandAxe; Command | 1 → 1 | — / — | 0 / 0 | — | — |
| 1100006 | HandAxe; Command | 9 → 9 | — / — | 0 / 0 | 1516 | — |
| 1100100 | HandAxe; Normal | 24 → 24 | SP:35 / -35 | 0 / 0 | 1171 | — |
| 1100300 | HandAxe; Parry | 1 → 1 | SP:30 / -30 | 0 / 0 | — | — |
| 1100400 | HandAxe; Dodge | 36 → 36 | SP:50 / -50 | 0 / 0 | 655 | — |
| 1100500 | HandAxe; Normal | 17 → 17 | SP:65 / -65 | 870 / 870 | 1196 | — |
| 1100510 | HandAxe; Normal | 8 → 8 | SP:55 / -55 | 0 / 0 | 5289 | — |
| 1100514 | HandAxe; LandingSkill | 5 → 5 | SP:30 / -30 | 0 / 0 | 1717 | — |
| 1100515 | HandAxe; Command | 42 → 42 | SP:20 / -20 | 1000 / 1000 | 1016 | — |
| 1100517 | HandAxe; Normal | 37 → 36 | SP:25 / -25 | 0 / 0 | 1719 | Skill_InCutScene×1 |
| 1100518 | HandAxe; Normal | 24 → 24 | SP:45 / -45 | 0 / 0 | 2360 | — |
| 1100528 | HandAxe; Normal | 6 → 6 | SP:85 / -85 | 0 / 0 | 1133 | — |
| 1101900 | HandAxe; SheathWeapon | 27 → 27 | — / — | 0 / 0 | 1672 | — |
| 1102000 | HandAxe; SwapWeapon | 3 → 3 | — / — | 0 / 0 | 206078 | — |

## Rejections, interpretation and remaining checks

**Observed:** The 21 matched failures are `Skill_Condition_NotEnoughCost`
**9** (skill 1418), `Skill_InReaction` **5** (1409, 1418, 1445),
`Skill_UnusableUtilityBag` **5** (item skill 1001),
`Skill_UnusableSkill` **1** (1423), and `Skill_InCutScene` **1** (1100517).
The 1423 failure is an official server-response instance; it does not
identify the cause of the separately reported local-client attack refusal.
Neither cooldown deadline nor minimum accepted gap establishes that a failed
request was rejected specifically for cooldown. No cooldown-specific
rejection was classified in this pass.

**Static/Observed agreement:** Every observed nonzero `53` SP delta equals
the catalog `SP` cost in its row. Three nonzero cooldown deadlines match
the catalog: **4,000 ms** (item skill 917), **870 ms** (1100500), and
**1,000 ms** (1100515). Their minimum accepted gaps are **32,158 ms**,
**1,196 ms**, and **1,016 ms**, respectively. Most other rows have zero
catalog and observed deadlines even when the next accepted use is much
later. Official-client traffic does not prove the emulator’s damage,
skill gating, or timer authority.

**Candidate/limits:** The unmatched 1420 request needs an individual
sequence check before assigning it a fate. Action/cancel/combo mechanics
can further restrict a nominally zero-cooldown skill; a minimum gap is
an upper bound on how fast this player happened to repeat it, not the
configured minimum. A receiver must enforce authoritative SP, accepted
cooldown, and contextual weapon/stance/target rules rather than trusting
client effect reports. Original-client acceptance against the current
emulator remains untested.
