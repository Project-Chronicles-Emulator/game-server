# Combat evidence checkpoint — captured demo client, 2026-10-24

**Scope:** protocol and static research, not an implementation or official balance specification.
Source: `protocol/opcodes.json`, `protocol/descriptors.normalized.pb`, the generated
`docs/protocol-feature-reference.md` combat/population/projectile/status sections,
`docs/capture-corpus.md`, and the three private `decoded-analysis.jsonl` files under
`out/captures/` (tutorial T, open-world P1, P2). Counts below are complete decoded
frames in that order. Build ID `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`;
client dump/catalog source SHA-256 `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`
(`data/catalog-manifest.json`). A native-log build ID is not the dump hash.
No runtime IDs, names, coordinates, tokens, or packet payloads are published.
**Observed** = present in these official-client logs; **Static** = matching dump
IDA or catalog; **Candidate** = not established by either. NPC/other-player
messages in the same log must not be attributed to the local player by count.

## Death, revival, and immediate builder contract

| Packet (direction/opcode) | Descriptor shape | T / P1 / P2 | Evidence |
| --- | --- | ---: | --- |
| `PS2C_DamageNoti` (server→client, **19**) | critical flags; damage type/origin; effect CMS; base/final amount; caster/base-caster/target IDs; reaction; tick; skill CMS/instance/effect IDs; repeated `StatChange` (`stat_type`, `source`, `instance_id`, `delta`, `result`, `tick_delta`) | **142 / 3,285 / 2,001** | Observed. All 5,428 carry `stat_changes`; amounts alone do not establish final HP. |
| `PS2C_UnitChangeStateNoti` (server→client, **32**) | `unit_id`, `tick`, `state`, `reason` | **54 / 1,053 / 618** | Observed 319 `Demise`, 643 Combat, 593 Idle, 126 Alert, 44 Return across the three logs. NPC deaths contribute. |
| `PS2C_DeathNoti` (server→client, **23**) | `base_caster_unit_id`, `target_unit_id`, `pos` | **11 / 209 / 101** | Observed; carries no revive delay or authoritative HP field. |
| `PC2S_ReviveReq` (client→server, **20039**) | `to_nui_flame:bool` | **0 / 1 / 3** | Observed, three false and one true. Request does not itself establish successful revival. |
| `PS2C_ReviveFailRes` (server→client, **24**) | `ec`, `death_delay_end_tick`, `pos` | **0 / 0 / 0** | Schema only; timing/error codes not observed. |
| `PS2C_ChangeStatNoti` (server→client, **47**) | `unit_id`, `tick`, repeated `stat_changes`, `default_source` | **40 / 1,247 / 647** | Observed vitals changes; match stat type before treating a change as HP. |
| `PS2C_StatSyncNoti` (server→client, **54**) | `unit_id`, repeated `flows`/`regens`, `tick` | **743 / 4,864 / 4,648** | Observed repeated sync; not a death or revive acknowledgment by itself. |

All four `ReviveReq` frames were preceded by a `DeathNoti` and `Demise` for
that death target within the same capture, at gaps **8,053 ms (P1)** and
**9,057 / 7,791 / 6,226 ms (P2)**. These are observed elapsed gaps, **not**
a proved minimum server revive delay; UI input timing is unknown. Following the
request, two sessions received `MapMoveStartNoti` at +22/+24 ms and
`MapMoveCompleteNoti` at +481/+525 ms; the other two received `TeleportReq`
at +22/+23 ms. Removal/addition and stat updates then appeared. None had
`ReviveFailRes`. Correlate by player unit identity, death target, character
state, map/instance, and authoritative HP before success. Reconnect/duplicate
revive and death-to-quest-credit behavior remain Candidate.

**Static IDA, read-only:** listed existing GUI instance `k7ui`, then obtained
`analysis_status` `finished=true`, `queue_empty=true`, 705,939 functions.
The `PC2S_ReviveReq` wrapper vtable `0x14A8217D8` has sender references at
`0x14637CEC0` and `0x14637CF40`: these set `to_nui_flame` to 0 and 1,
respectively, and enqueue through `0x14637AB80`; `0x14637CD60` conditionally
constructs a false request. This verifies both wire choices, **not** server
revive acceptance logic. No IDA names, comments, or database changes in this
read-only checkpoint; previously saved annotations remain untouched.

## Player and monster skills, AI selection

| Packet (direction/opcode) | Descriptor shape | T / P1 / P2 | Evidence |
| --- | --- | ---: | --- |
| `PC2S_StartSkillReq` (client→server, **20010**) | `equip_slot`; `start_skill` (`src_skill_id`, `skill_id`, `instance_id`, `layer_index`, `pos`, `rotation`, `tick`, optional targeting yaw); repeated `target_ids`; `use_item`; command/lock-on target IDs | **226 / 949 / 1,061** | Observed client skill requests, not proof of hit. |
| `PS2C_SkillUtilizationCostNoti` (server→client, **53**) | `skill_instance_id`, `tick`, repeated `cooldowns`, `costs`, `stat_changes` | **225 / 933 / 1,056** | Observed server costs/cooldowns; client reports cannot own cost. |
| `PS2C_StartSkillNoti` (server→client, **14**) | `unit_id`, `start_skill`, `weapon_state_type`, `weapon_slot_type`, `keep_crouching` | **281 / 3,160 / 2,865** | Observed for multiple units; caster identity must be classified. |
| `PC2S_ExecuteSkillEffectReq` (**20011**) / `PC2S_ChangeSkillStageReq` (**20013**) | effect: `tick`, `skill_instance_id`, `pos`, `rotation`, `skill_effect`; stage: instance, stage index, reason, pos/rotation/tick | **112/29/51**; **4/98/82** | Observed client stage/effect reports; do not trust them to award damage or shorten timers. |
| `PS2C_ChangeSkillStageNoti` (**17**) / `PS2C_EndSkillNoti` (**15**) | stage: unit/skill/instance/stage/pos/rotation/tick/reason; end: unit + `EndSkill` (`instance_id`, `pos`, `yaw`, `tick`, `reason`) | **2/101/23**; **34/538/498** | Observed server-owned transitions/endings. |
| `PS2C_StartSkillFailRes` (**13**) / `PS2C_ExecuteSkillEffectFailRes` (**18**) | start: `ec`, skill/weapon/instance/target, internal flag; effect: instance, stage, failed effect | **1/16/4**; **2/0/2** | Observed errors; exact validation precedence remains Candidate. |
| `PS2C_ChangeNpcAggroTopTargetNoti` (**98**) | `unit_id`, `new_target_unit_id` | **22 / 2,092 / 727** | Observed target acquisition/clear (set/clear 11/11 T, 1,866/226 P1, 616/111 P2). Not a complete threat table. |
| `PS2C_SetNpcAiPhaseNoti` (**114**) | `unit_id`, `new_ai_phase_transition_id` | **32 / 912 / 440** | Observed phase IDs and omission of optional ID; numbers alone do not specify threat/skill choice. |
| `PS2C_NpcMotionEndSyncNoti` (**106**) | skill instance/layer/stage, end yaw/tick, unit, destination | **11 / 146 / 89** | Observed NPC stage/motion completion. |

Matching request `start_skill.instance_id` and skill CMS ID against subsequent
`StartSkillNoti` within 1.5 s yielded **225/226 T, 933/949 P1, 1,056/1,061 P2**
paired requests; median log gaps **21/25/65 ms**. Matching cost by instance ID
produced **225/933/1,056** pairs and the same median gaps. These timings are
sample-local associations, not formal skill latency or universal packet order;
other units interleave. One sanitized P1 sequence: request at 0 ms, cost and
start notification at +74 ms, client projectile-effect request +408 ms, spawn
notification +429 ms. An adjacent damage event cannot be attributed to this
request without matching skill instance, caster, and target.

Classifying a `StartSkillNoti` only while its unit is in the captured
`UnitAddNoti.nub.npc` roster gives an **observed lower bound** of NPC-cast
starts **42/831/597** (distinct NPC skill IDs **30/94/112**). Many remaining
starts are unresolved because a unit can predate the captured spawn or be
removed; this is not the total NPC attack count. Catalog `Npc.jsonl` joins
`NpcTemplate.jsonl` by `FK_template`; sample observed NPC CMS 5001028 joins
template 36171 (`ai=1200`, suitable combat distance 50–300 catalog units,
phaseSkill 361711). That actor emitted skill CMS 361710200 42 times and
361710100 32 times in P1. CMS 5001026/template 36201 emitted skill
362010700 39 times. Template `FK_usableSkills` is empty for these two:
**do not treat it as the complete NPC attack list**. The underlying AI choice
weights, range units, reaction to threat, and phase-transition conditions
are Candidate until catalog scripts/IDA handlers are resolved. `Skill.jsonl`
contains per-skill costs, cooldown, target geometry and distance fields;
`SkillEffect.jsonl` contains effect targeting/damage coefficients; defaults
are definitions, not measured server decisions. Catalog provenance is
`data/catalog-manifest.json`; do not transplant across builds by ID alone.

## P0: “Cannot use this skill” — identity of the displayed error

**Observed screenshot:** `out/agent-team-reports/20261024-standing-order/cannot-use-skill.png`
shows a staff-wielding character in the tutorial and the system text.
The image alone contains no packet, skill ID, server error code, or proof
of whether the client sent a request. Do not assume the current deployed
server sent a particular error without correlating its private logs.

**Exact localized error:** matching-build
`G:/Dumps/Archeage/ClientData_2026-10-23/raw/X5/Content/Design/CMS/en/L10N.csv`
maps `TXT_ERRORCODE_0000005121_MESSAGE` to **“Cannot use this skill.”**
`protocol/descriptors.normalized.pb` maps
`x5.ec.EErrorCode` numeric **5121** to **`Skill_UnusableSkill`**.
The rendered text is in a localization asset, not a literal string
found in the analyzed client executable, so a string xref in the IDB
cannot by itself locate the display path.

**Observed official server-response proof, not this screenshot's cause:**
in `out/captures/generic-part-1-through-level-6-20261024-153513/decoded-analysis.jsonl`,
a `PC2S_StartSkillReq` **20010** for skill CMS **1423** is followed
**22 ms** later by `PS2C_StartSkillFailRes` **13** with matching
`skill_id` and `instance_id`, `ec=Skill_UnusableSkill`. That capture
is of a **Wild** character, not the reported Magic/staff character.
**Static IDA:** packet factory `0x1461A1E60` (wrapper vtable
`0x14A825E58`) and callback `0x1461E2E80`, registered near
`0x1461C24A0/0x1461C2537`. The callback reads the response error
at offset +32, calls the generic localized error display
`0x145CCE2F0`, and resets local skill state. The callback is saved
as `AAC_PS2C_StartSkillFailRes_DisplayErrorAndResetSkill` in
`../dumps/ArcheAgeChronicles.memdump.exe.i64`, with pseudocode
and journal under `../dumps/ida_research/combat-20261024/`.
Thus **a server StartSkillFailRes with ec 5121 can produce this exact
string**, but that historical Wild failure is **not** the reported
Magic/staff attempt. **Builder-reported live evidence (PID 53460,
deployed `ccff5d0`):** its complete `server.log` has **zero**
`StartSkillReq` **20010** and zero `StartSkillFailRes` **13**
for the pictured Magic/class-5 session through the screenshot time
20:17. The other class-3 session has the only live request at 19:03,
accepted with cost **53** and start **14**. The deployed source has
no `Skill_UnusableSkill`/5121 emission path. These reported observations
exclude a server response to the **pictured** attempt, provided the
log covers that connection; client-local refusal or a UI state/binding
issue is now the leading explanation, **not yet a traced precheck**.
A paired client-side action trace must identify the exact local gate,
attempted skill, equipment and state before changing combat validation.

**Observed successful Magic/ElementalStaff example:** the older
`G:/Dumps/Archeage/ClientData_2026-10-23/LivePackets/Decoded/packets.decoded.jsonl`
contains **17,055** decoded records from `native-log-final.bin`,
SHA-256 `96b14c13062dce6c78f9396271cb66c8ce7d9e0dab140a02192e6af14e856240`,
native build ID `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`.
This first captured staff attack belongs to the **historical buffered
portion**, not the 240 records appended during that read-only trial.
`MapMoveCompleteNoti.nub.player.char_class=Magic`; its equipment
includes catalog item **11104102** (`Item.equipmentType=ElementalStaff`,
`equipSlot=Weapon_1`). `Skill.jsonl` calls **2220** an
ElementalStaff basic attack with `weaponStateType=Drawn`. The earliest
captured `PC2S_StartSkillReq` for **2220** contains
`start_skill.{skill_id,instance_id,pos,rotation,tick}`; no `equip_slot`,
target list, or lock-on field is encoded in that instance. At **+63 ms**
the matching `skill_instance_id` appears in
`PS2C_SkillUtilizationCostNoti` **53**, then
`PS2C_StartSkillNoti` **14** with the same skill/instance and
`weapon_state_type=Drawn`. The cost packet has one cooldown entry
and SP stat change; no `StartSkillFailRes` for that instance.
Its request, cost, and start-notification `tick` values match;
between later recorded requests, client tick deltas are approximately
**1,000,000 per logged millisecond** (nanosecond domain), not
100-nanosecond ticks. This is official-client behavior, **not** proof
that the current server accepts skill 2220 or the reported tutorial
character has the same weapon/state. Builder inspection of the shared
generated profile confirms **2220** is available to class **5** with
`ElementalStaff`, no required target, SP cost **5**, cooldown **0**.
That catalog eligibility makes a paired client trial useful, but does
not establish that this character currently has the skill bound,
the staff equipped/active, or a valid client animation/state.

## P0 IDA: client-local skill/attack gate producing 5121

**Correction (2026-10-25):** a later static pass changed the reading of this gate.
`sub_146044C80` returns true unless the skill is perk-gated and the gate fails; the
`+376`/`+377` bytes are build-point-remaining flags; and the emulator's world-entry 135
already carries the class's catalog `autoRegistration` perks at level 1, like the official
captures. The leading explanation below (a missing 135) is therefore refuted. See
[combat-skill-refusal-evidence.md](combat-skill-refusal-evidence.md) for the corrected gate,
the 21 official refusals and the remaining live-trace questions.

**Confirmed by static analysis (instance `k7ui`, build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`):**

The client's skill eligibility function is `sub_14623F3E0` (called from the
skill-use path). It returns error code **5121** (`Skill_UnusableSkill`) at
`0x14623f4b0` when `sub_146044C80(actor, &skill_id)` returns 0.
`sub_146044C80` checks whether the skill ID exists in a **perk-gated skill
list** stored in the `FX5PerkManager` singleton (`sub_1460442B0` →
`FX5PerkManager`). The check iterates perk entries; for each entry it looks
up a skill list and searches for the requested skill ID. If the skill is not
found in any perk-gated list, it falls through to a second check
(`sub_145DC3580`) against a default/unlocked skill set. If neither contains
the skill, `sub_146044C80` returns 0 → 5121.

**Perk state source:** `sub_145F28CB0` (handler for PS2C_WorldCharPerkNoti,
opcode 135), registered via vtable `0x14A8280A8`, factory `0x1461A4B90`,
callback `sub_1461E0350`. It populates:
- `*(actor+376)` and `*(actor+377)` — two boolean flags (perk type 0 vs 1).
- A hash map at `actor+520`/`+528`/`+536`/`+580`/`+592`/`+600` mapping
  perk IDs to skill ID lists.
- Calls `sub_145F238E0` to update additional perk-derived state.

**Weapon-swap path (separate):** `sub_146048B30` returns 5121 when
`sub_14604B610` returns false (skill/perk mismatch) and the skill is not
already active (`sub_1462619A0` returns false). This is the weapon-swap gate,
not the basic-attack gate.

**Conclusion:** The client's local 5121 check **does** read perk state
populated by the 135 handler. If the server does not send 135 with the
correct perk→skill mappings, the client will reject skill use with 5121
**without sending a request**. This matches the observed live behavior
(PID 53460: zero StartSkillReq for the pictured session).

**Traits UI:** The perk manager (`FX5PerkManager`, `sub_1460442B0`) is the
data source. Perk entries contain skill lists and a byte at offset +9
(perk type). The Traits window ("Axe (Equipped)", "0/0 points") reads from
this manager. The `sub_145F29120` function checks whether a perk's skill
list contains a specific skill ID with matching perk type byte.

**Weapon draw/stance gate:** `sub_14623F3E0` checks `*(skill+10) == 5`
(weapon class) and calls `sub_14613C520`/`sub_146149750` to validate
weapon-slot compatibility. If the weapon is not drawn/active, 5164 is returned.
The weapon state is synced from the Nub via `AAC_WorldActor_ApplyPlayerNubAttributes`
(`0x146044780`) which calls `sub_146048B30` with `a3=0` (no display).

**Candidate function names (to be saved in IDB):**
- `sub_146044C80` → `AAC_Perks_CheckSkillUnlocked` (Candidate)
- `sub_14623F3E0` → `AAC_Skill_CheckEligibility` (Candidate)
- `sub_146048B30` → `AAC_Weapon_CheckSwapEligibility` (Candidate)
- `sub_145F28CB0` → `AAC_WorldCharPerkNoti_ApplyPerkState` (Candidate)
- `sub_1460442B0` → `AAC_FX5PerkManager_Get` (Candidate)
- `sub_145F26BE0` → `AAC_Perks_GetSkillOpenState` (Candidate)
- `sub_145F29120` → `AAC_Perks_FindSkillInPerkList` (Candidate)

**Next step:** Name these functions in IDB, save, and update docs/combat-evidence.md
with the confirmed findings. Then report to builder and chief.

**IDB status:** All 7 candidate functions renamed successfully (ok:true).
`idb_save` timed out twice (60s each); the renames are in IDA memory but
the database file may not be flushed. The instance appears unresponsive
after the save attempts. A retry or manual save is needed.

## P0 capture-to-catalog cross-reference (no IDA)

**Method:** Extracted all `PC2S_StartSkillReq` skill IDs from the three new
captures (docs/capture-corpus.md) and compared against `PerkNode`
`autoRegistration=true` + `jsonCase=SkillOpen` rows in the client catalog.

**Captured skill IDs (all classes combined):** 40 unique skill IDs across
3 captures (generic-part-1: 31, generic-part-2: 34, full-tutorial: 16).

**Cross-reference results:**

| Class | Granted (autoReg SkillOpen) | Captured (StartSkillReq) | Used but NOT granted | Granted but NOT captured |
|-------|---------------------------|------------------------|---------------------|------------------------|
| Fight | 45 | 0* | 0 | 45 |
| Wild | 37 | 24 | 0 | 13 |
| Adamant | 34 | 0* | 0 | 34 |
| Magic | 47 | 0* | 0 | 47 |

*Fight/Adamant/Magic StartSkillReq packets were not captured in these three
files; only Wild-character skill use appears in the decoded StartSkillReq
records. The older `LivePackets/packets.decoded.jsonl` contains Magic skill
2220 (ElementalStaff basic attack) but is not part of the three new captures.

**Wild class detail (the only class with captured StartSkillReq):**
- 24 of 37 granted skills were observed in use.
- **0 skills used but not granted** — every captured Wild skill maps to an
  autoRegistration SkillOpen node.
- 13 granted skills not captured (likely higher-level or situational skills
  not exercised in these captures): 1425, 1100008, 1100516, 1110007-1110050.

**Common/unmapped captured skills (16):** 900, 917, 1000, 1001, 1003, 1327,
1401, 1417, 1445, 1446, 1462, 1463, 1100100, 1100518, 1101900, 1102000.
These are not found in any class's SkillOpen autoRegistration set. They may be
universal/basic skills, NPC skills, or skills granted by a different mechanism
(e.g., quest rewards, default class kit). Further investigation needed.

**Conclusion:** For the Wild class, the catalog's autoRegistration SkillOpen
nodes **fully cover** all observed skill use (0 gaps). For Fight/Adamant/Magic,
no StartSkillReq was captured in these three files, so the hypothesis that
135 (WorldCharPerkNoti) built from autoRegistration nodes provides sufficient
skill coverage **cannot be verified** for those classes from these captures
alone. The older LivePackets capture shows Magic skill 2220 is used and is
granted by autoRegistration node 1425101, consistent with the hypothesis.

**Sanitized:** No player/session values, chat text, or credentials included.

## P0 unmapped captured skills (16) — mechanism analysis

Each of the 16 captured skills not found in any SkillOpen autoRegistration set
was traced through the client catalog:

| Skill ID | Comment (Korean) | skillType | Grant mechanism | Needs 135? |
|----------|-----------------|-----------|-----------------|------------|
| 900 | Ice wedge throw (item) | Item | Item FK_skillId | No — item-granted |
| 917 | Stamina potion | Item | Item FK_skillId | No — item-granted |
| 1000 | Potion consume | Item | Item FK_skillId | No — item-granted |
| 1001 | Throwing dagger | Item | Item FK_skillId | No — item-granted |
| 1003 | Poison flask throw | Item | Item FK_skillId | No — item-granted |
| 1327 | Wild: bow draw (swap) | SwapWeapon | WeaponSkills.swapWeaponSkillId | No — weapon-granted |
| 1401 | Wild: bow sheath | SheathWeapon | WeaponSkills.sheathWeaponSkillId | No — weapon-granted |
| 1101900 | Wild: axe sheath | SheathWeapon | WeaponSkills.sheathWeaponSkillId | No — weapon-granted |
| 1102000 | Wild: axe swap | SwapWeapon | WeaponSkills.swapWeaponSkillId | No — weapon-granted |
| 1100100 | Wild: axe basic attack (combo) | Normal | SkillChanger src_skillId | No — combo-granted |
| 1417 | Wild: bow aimed shot (combo) | Normal | Combo chain (1463→1417) | No — combo-granted |
| 1445 | Wild: bow rapid fire prep | Normal | Combo chain | No — combo-granted |
| 1446 | Wild: bow rapid fire shot | Normal | Combo chain | No — combo-granted |
| 1462 | Wild: bow strong attack (combo) | Normal | SkillChanger src_skillId=1463 | No — combo-granted |
| 1463 | Wild: bow strong attack (command) | Command | SkillChanger src_skillId=1463 | No — combo-granted |
| 1100518 | Wild: axe throw (combo) | Normal | Combo chain | No — combo-granted |

**Grant mechanisms identified:**
- **Item FK_skillId** (5 skills): Item use skills triggered by consuming/throwing items.
- **WeaponSkills table** (4 skills): Sheath/swap weapon skills defined per weapon type.
- **SkillChanger table** (2 skills): Combo/chain skill transformations.
- **Combo system** (5 skills): Implicit combo skills chained from base skills.

**Answer: Does our 135 implementation need anything beyond autoRegistration SkillOpen/WeaponUsable?**

**No.** All 16 unmapped captured skills are granted by non-perk mechanisms
(items, weapon definitions, combo chains, SkillChanger). None of them appear in
the perk-gated skill lists, and none of them would be blocked by the client's
5121 check if the perk lists were the only source. The 135 (WorldCharPerkNoti)
implementation needs only the autoRegistration SkillOpen and WeaponUsable nodes
to satisfy the client's local skill eligibility gate.

**Sanitized:** No player/session values, chat text, or credentials included.

## Projectiles, defense, effects, PvP

| Path (direction/opcode) | Shape and observed T / P1 / P2 | Evidentiary limit |
| --- | --- | --- |
| `PC2S_ExecuteSpawnProjectileSkillEffectReq` **20012** → `PS2C_SpawnProjectileNoti` **40** | effect request: tick, skill instance, pos/rotation/effect (**58/754/821**); spawn: caster unit + `SpawnProjectile` (**80/1,316/895**). Spawn DTO has projectile CMS, instance, target, spawn tick, duration and endpoints. | Observed; server spawn and collision still require ownership and instance matching. |
| `PC2S_ClientProjectileHitReq` **20021** → `PS2C_ProjectileHitNoti` **42** | client: elapsed milliseconds, instance ID, `SkillObjectHit` (**93/973/1,115**); server: caster, instance, repeated hit target data (**54/1,015/664**). | Observed, but not a proof client reported hit is authoritative. |
| `PC2S_DespawnClientProjectileReq` **20022** / `PC2S_UpdateProjectileReq` **20023**; `PS2C_DespawnProjectileNoti` **44** / `PS2C_UpdateProjectileNoti` **46** | client **58/750/816**, **271/3/0**; server **80/1,361/963**, **50/18/61**. Nested `DespawnProjectile`/`UpdateProjectile` carry details. | Observed lifecycle; ordering and collision decision require instance-correlated trace. |
| `PS2C_DamageNoti` **19** / `PS2C_ImmuneNoti` **20** / `PS2C_ReactionNoti` **61** | **142/3,285/2,001** / **28/360/208** / **1/5/3**; damage type explicitly Shield **60** and Guard **16** across all captures; `ImmuneNoti.dodge=true` **18** times; 1,039 damage notifications carry `Reaction` (`reaction_type`, start/end ticks and trajectory). | Observed guard/shield/dodge signals; omission of a damage type does not prove no mitigation. Block formula, crit and defense precedence Candidate. |
| `PS2C_AddStatusEffectNoti` **49**, `UpdateStatusEffectNoti` **50**, `RemoveStatusEffectNoti` **51** | **54/657/607**, **92/809/711**, **58/674/619**; unit + `StatusEffect` (CMS, instance, level/stack, caster, start/expire ticks), repeated updates/removals. | Observed lifecycle; stack/expiry authority and buff mechanics Candidate. |
| `PS2C_ChangeAbnormalStatusGaugeNoti` **52**, `PS2C_AddStatRecoveryNoti` **80** | **3/17/10**, **3/21/20**. | Observed; `HealNoti` **21** and aura packets **108–113** have **0** in all three captures; schema only, not unsupported. |
| PvP actor-vs-actor damage | No `DamageNoti` was classified as known-player→known-player while both identities were present in the capture's `UnitAddNoti.nub.player`/map-complete roster. Classified known player→NPC: **0/1,323/824**; NPC→player: **0/81/75**. | **Candidate PvP:** 142/1,880/1,102 damage frames lacked sufficient in-window identity classification, so PvP absence cannot be asserted. PvP target/faction/group legality must be server-owned. |

## Death and revival implementation specification: four player-correlated cases

Each case below joins `DeathNoti.target_unit_id` (decoded as a decimal string)
to the latest `MapMoveCompleteNoti.player_unit_id` (decoded as an integer) after
numeric normalization. The same player receives `UnitChangeStateNoti` **32**
with `Demise` at the death timestamp. `DeathNoti` **23** follows in the same
millisecond (within 1 ms in one P2 case), and `ChangeStatNoti` **47** follows
with a shield stat change. The four `ReviveReq` **20039** offsets below are
relative to each request, not to the capture start. Events at the same logged
millisecond have no guaranteed network order.

| Case; prior player death | Request option | Server/client sequence after request | Destination evidence |
| --- | --- | --- | --- |
| P1; −8,053 ms | `to_nui_flame=false` | +21 ms server stat HP=248/SP=567; +22 ms `MapMoveStartNoti` **175** and `ItemsUpdateNoti` **169** with `death` money change −55 Gold; +481 ms `MapMoveCompleteNoti` **176** | Map ID changes; destination is **0.1 m** from a static `nuiIdol` spawn position. |
| P2 case 1; −9,057 ms | `false` | +22 ms server `TeleportReq` **181**; +29 ms client `TeleportStartReq` **20088**; +60 ms client `TeleportCompletionReq` **20089**; +83 ms remove/add roster; +123 ms player HP=257/SP=586, −90 Gold, `Idle` state | In-map teleport; destination **2.0 m** from a `nuiIdol` spawn position. |
| P2 case 2; −7,791 ms | `true` | +23 ms server `TeleportReq`; +24 ms client start; +90 ms remove/add roster; +492 ms client completion; +517 ms player HP=257/SP=586, −90 Gold, `Idle` | In-map teleport; destination **0.4 m** from a `nuiFlameObject` spawn position; nearest idol is 33.6 m away. |
| P2 case 3; −6,226 ms | `false` | +24 ms player HP=257/SP=586, `MapMoveStartNoti`, −90 Gold; +525 ms `MapMoveCompleteNoti`; +1,172 ms player `Idle` | Map ID changes; destination within capture precision of a `nuiIdol` spawn position. |

**Observed + matching-build static asset join:** The `false` option reaches
three `nuiIdol` spawn positions; the sole `true` reaches a `nuiFlameObject`
spawn position. `false` yields **both** cross-map and in-map transports:
selection of shrine type does not itself select the transport opcode.
The client receives either `MapMoveStartNoti.spawn_point` or
`TeleportReq.pos`, then a spawned state/roster. Four aligned positions
are strong evidence for idol-versus-flame semantics in the captured
areas; nearest-point/activation rule, unattained shrines, and fallback
when no shrine is available remain **Candidate**.

**Provenance and coordinate conversion:** All four destination map IDs
resolve in `data/generated/catalog/Tables/Map.jsonl`, although that
table and `Zone.jsonl` have no shrine placement coordinates.
`MapMarker.jsonl` defines the `NuiIdol` and `NuiFlame` icon types.
Actual matching-build placements are in the non-catalog raw export:
`data/generated/raw/X5/Content/Data/BuiltData/Maps/*/DataLayers/*/*/spot_object.json`.
Each `adventureObject.nuiIdol.spawnVolume.spawnVolumeSphere.spawnPosition`
or `nuiFlameObject.spawnVolume.spawnVolumeSphere.spawnPosition` is in
client-world centimeters; multiplying all axes by 10 aligns the
observed `WorldPos.millimeter_{x,y,z}`. Scanning 221 spot-object files
yielded 195 idol and 101 flame spawn spheres. The nearest idol distances
for the three false cases are 0.1/2.0/0.0 m; the true case is 0.4 m
from a flame (nearest idol 33.6 m). These are **spatial joins**, not proof
that a client-selected shrine ID was transmitted; the request has only
the Boolean option. Keep all raw positions and capture IDs private.
`GameVariable` keys `ActivateResurrectionPointInteractionCmsId=10`,
`NuiFlameDistance=20000`, `NuiFlameInteractionGroupId=27`, and
`NuiFlameObstacleModelId=10071` support the shrine classification,
but do not establish access/range policy. `ReviveTask.jsonl` defines
item resupply, not a spawn table; `LobbySpawnPoint.jsonl` is only for
lobby offsets. No `ReviveFailRes` **24** was observed; its descriptor
has `ec`, `death_delay_end_tick`, and `pos`.

**Observed vitals:** `EX5StatType_HP=69`, `SP=72`, catalog/map maximum
HP/SP stat IDs **18/19**. Each post-request player `ChangeStatNoti` reports
HP/SP equal to that case's preceding map snapshot maximum: **248/567** in
P1; **257/586** in all three P2 cases. This is four full-vitals examples,
not a proven universal restoration rule. Death-to-request elapsed times
**6.2–9.1 seconds** include human UI latency and do not establish a
minimum server revive timer. `ReviveFailRes.death_delay_end_tick` does have
a client-side consumer (static finding below); no captured failure establishes
which error code or delay the official server sends.

**Observed penalties:** At the *death* timestamp, `ItemsUpdateNoti` updates
item durability. Comparing each item's last seen durability to the same
item in the death update: P1 had six −300, four −60, two −4 changes;
P2 cases had nine −300 and respectively one/four/four −60 changes.
**Static catalog:** `GameVariable.DurabilityDeathDecreamentValue=0.03`.
The −300 changes belong to catalog items with `Item.maxDurability=10000`;
the −60 changes belong to max 2000 items. Both equal **3% of maximum**
durability, not 3% of current durability. The two P1 −4 changes are on
max-10000 items and are inconsistent with the death rule: adjacent ordinary
combat wear remains the likely but unproved explanation. Do not apply
an extra −4 death penalty. The rounding/clamping rule at low durability
and special item categories remain **Candidate**. Catalog
`DeathPenaltyExpDecreamentValue=0` is static configuration for this build.
No `ChangeAdventureExpNoti` **38** appeared from 200 ms before each death
through 1.3 s after revival; experience penalties outside that window
are **Candidate**, not disproved. Each case has exactly one
`ItemsUpdateNoti.death.money_changes` **at revival**, Gold −55 (P1,
level 5) or −90 (P2, levels 7/8). The earlier separate −35 example in
`docs/inventory-equipment.md` is not any of these four cases. Do not
hardcode a level-only formula or deduct gold at both death and revival.
Money type is omitted on these decoded messages; `EX5MoneyType_Gold=0`
is its schema default. No local `ChangeNpcAggroTopTargetNoti` was present
within 150 ms before through 600 ms after any of the four deaths or
the first 1.5 s after revival; explicit aggro clearing is **Candidate**.

**Static IDA (saved):** Existing instance `k7ui` reported an empty analysis
queue and 705,939 functions. `AAC_PC2S_ReviveReq_SendFalse` at
`0x14637CEC0` and `AAC_PC2S_ReviveReq_SendTrue` at `0x14637CF40`
construct opcode **20039** with `to_nui_flame` bytes 0 and 1.
`AAC_ReviveUI_Candidate_DispatchChoice` at `0x14637CFC0` routes
three UI choice bytes to the two senders and an alternate debug-gated
false sender; visible choice labels remain unknown. Factory
`0x1461A0C10` constructs the `ReviveFailRes` wrapper (vtable
`0x14A826170`); transport registration at `0x1461C301A` and
`0x1461C30B1` associates it with
`AAC_PS2C_ReviveFailRes_Candidate_ApplyPositionAndRetryTick`
(`0x1461E16F0`). That callback consumes the decoded position, converts
`death_delay_end_tick` to a local time value, **adds 10,000,000 local
ticks**, and updates a local player component. This is **client UI handling
after failure**, not evidence that the server's minimum wait is one second;
the clock conversion and callback association are marked Candidate.
The callback does not visibly branch on `ec`. The descriptor's
`Resurrection_*` errors (11100–11104) concern resurrection-point
activation by name, **not** verified `ReviveReq` failure values.
Function index, cached pseudocode and append-only journal:
`../dumps/ida_research/combat-20261024/`. The existing `.i64` was saved
successfully at `../dumps/ArcheAgeChronicles.memdump.exe.i64`.


**Server contract (implementation guidance, not observed mechanics):**
Check player identity, same live session/map, actual Demise state, and
server-owned revive eligibility/timing; never accept client coordinates,
HP/SP, or a revival while alive. Select the server-resolved destination,
apply any validated durability/money penalty **once per death revision**,
restore authoritative vitals and state, and send coherent transport,
inventory, and stat updates. Persist consequences atomically before
success; reconnect and duplicate requests must not duplicate losses.
Reject invalid requests with a confirmed `ReviveFailRes.ec` when the
error semantics are known; neither the matching error code nor timer
policy is established by these four successful trials.

### NPC selection, cooldown and range: measured vs catalog

**Static catalog:** `Npc.jsonl` identifies a template and sight/alert/
return distances; `NpcTemplate.jsonl` has `ai`, `phaseSkill`,
`FK_usableSkills`, and `combatSuitableDistMin/Max`; `Skill.jsonl`
defines `cooldown`, `cooldownGroup`, `minDistance`, `maxDistance`, and
costs. In P1 a spawned NPC template 36171 (suitable distance 50–300
catalog units) casts CMS skill 361710200 **42** times
(`Skill.cooldown=4000`, range 0–300), and 361710100 **32** times
(`cooldown=0`, range 0–150). Another spawned template 36201 casts
362010700 **39** times (`cooldown=5000`, range 0–500). Both templates
have empty `FK_usableSkills` yet cast these skills: that field is **not**
a complete selection table; investigate `phaseSkill`/AI scripts before
filtering out observed actions. Distances' units and skill-vs-template
range precedence remain **Candidate**. NPC movement speed provenance and the
inconclusive capture measurements are in
[npc-speed-provenance.md](npc-speed-provenance.md).

**Observed repetition, not a cooldown proof:** Repeated starts of
361710200 by the same unit in P1 yield ten logged inter-start gaps
under 60 s, median **4,400 ms**, with **one under** the catalog 4,000.
For 362010700, thirteen gaps have median **6,108 ms**, with **three
under** the catalog 5,000 (three of these reuse the same skill instance).
Stage/instance reuse, buffs, interrupted actions, and actor lifetime can
confound those gaps; do not treat the median as the server timer. Position
snapshots are insufficient to establish target distance at all cast times.
Choose only server-owned eligible skills and schedule cooldown/range
after validating catalog semantics; do not infer a phase/skill priority
or probability from aggregate counts. A next bounded trace should
correlate actor spawn, aggro target, phase transition, skill instance,
target movement, and catalog skill definition for one NPC across attacks.

## Implementation gaps to prioritize

1. **Revival:** death-state ownership, HP restoration, server-side delay and
   spawn/teleport branch, repeated/reconnect requests. Four observed request
   sequences constrain ordering, not exact delay policy. No accepted revive
   response opcode is established in this subset.
2. **Monster skill choice:** spawner/NPC template identity, phase/target/range
   and selected skill CMS must be resolved together. Existing template
   `FK_usableSkills` cannot stand in for observed attacks. Capture-derived
   NPC skill counts are lower bounds, not AI probabilities.
3. **Combat authority:** player and mob damage, defense, projectiles, effects,
   cooldowns and PvP require matching action instances and server simulation.
   A client hit/effect report cannot grant damage or rewards. Shared-world
   spectators and correct ordering need a paired original-client trial.
