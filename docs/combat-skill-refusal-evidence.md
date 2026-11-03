# Wild skill refusal evidence

This study was written on 2026-10-25 against the cost study in
[combat-cost-cooldown-evidence.md](combat-cost-cooldown-evidence.md) and recovered from an
archived worktree on 2026-10-27. Its captures, catalog rows and IDA findings are unchanged.
Source line numbers below refer to `eb0834b` and are historical.

## Status against current source

- `CombatSession::start` (`src/world/features/combat/combat.cpp`) now answers an SP shortage with
  `Skill_Condition_NotEnoughCost`, the observed code; an HP-cost shortage still uses
  `Skill_Condition_NotEnoughSP` because no official HP refusal was captured.
  `Combat.CatalogSkill1418RefusesInsufficientStamina` is the deterministic SP 9 / SP 10
  check proposed below.
- `Skill_InReaction`, `Skill_InCutScene`, `Skill_UnusableUtilityBag` and
  `Skill_UnusableSkill` still have no server path, and their official predicates remain
  unknown. Do not add them by guessing the state that triggers them.
- The client perk-gate section below corrects the earlier summary in
  [combat-evidence.md](combat-evidence.md) ("P0 IDA: client-local skill/attack gate producing
  5121"). The function names are saved in the matching IDB with a journal under
  `../dumps/ida_research/combat-20261024/`.

## Checkout and provenance

- Base: cost study `e4a6a2a9a4e20b561ea9b1c9826de858589c4454`, which changes only
  `docs/combat-cost-cooldown-evidence.md`. The study used the isolated worktree
  `out/worktrees/combat-refusal-e4a6a2a` based on that commit.
- **Observed:** private 2026-10-24 captures `full-tutorial-capture-20261024-152100` (T), `generic-part-1-through-level-6-20261024-153513` (P1), and `generic-part-2-20261024-162411` (P2); capture build ID `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. Capture hashes/counts are in `docs/capture-corpus.md`. No player/unit/session IDs or position coordinates are published.
- **Static:** `data/generated/catalog/Tables/Skill.jsonl` for skill costs and cooldown values. `SP:<n>` names the catalog cost; packet `PS2C_SkillUtilizationCostNoti` **53** stat type 72 subtracts exactly that amount on observed acceptances. Nonzero catalog cooldown values 870, 1000, 4000 equal `(53.cooldowns.expire_tick - 53.tick) / 1,000,000`, establishing catalog cooldown unit **milliseconds** against nanosecond wire ticks. Stat type 72 as SP remains inferred by matching values, not an independently decoded enum.

## Matching and recorded refusals

**Observed:** Match `PC2S_StartSkillReq` **20010**
(`start_skill.skill_id`, `start_skill.instance_id`) to
`PS2C_StartSkillFailRes` **13** (`skill_id`, `instance_id`, `ec`) in each
capture. All 21 matched failures followed the request by 21–66 ms.
Table source locations are *private decoded file line numbers* only; they
do not identify players. `Prior SP` compares the latest preceding opcode
53 stat-type-72 **result** to the catalog skill cost. This result is a
potentially stale observation, not a guaranteed server SP snapshot at
refusal; opcode 54 regen flows can change it. `Prior use gap` measures
milliseconds since the last matched acceptance of that skill in this
capture, not a cooldown deadline. `Target` means a target reference was
supplied, **not** confirmed range, ownership, visibility, or target
validity. All six affected skills have catalog cooldown zero; no active
same-skill wire deadline was established for any refusal.

| Capture:decoded line | Skill; catalog cost | Response `ec` | Prior SP vs cost | Prior use gap ms | Target supplied |
| --- | --- | --- | --- | ---: | --- |
| T:3784 | 1100517; SP:25 | `Skill_InCutScene` | at least | 2338 | yes |
| P1:24502 | 1445; none | `Skill_InReaction` | not applicable | — | yes |
| P1:28018 | 1001; none | `Skill_UnusableUtilityBag` | not applicable | 929 | yes |
| P1:28366 | 1001; none | `Skill_UnusableUtilityBag` | not applicable | 29565 | yes |
| P1:28369 | 1001; none | `Skill_UnusableUtilityBag` | not applicable | 31290 | yes |
| P1:30066 | 1409; SP:50 | `Skill_InReaction` | at least | 39647 | yes |
| P1:31313 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 3973 | yes |
| P1:31316 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4121 | yes |
| P1:31318 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4153 | yes |
| P1:31320 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4187 | yes |
| P1:31322 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4220 | yes |
| P1:31325 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4304 | yes |
| P1:31327 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4337 | yes |
| P1:31329 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4370 | yes |
| P1:31331 | 1418; SP:10 | `Skill_Condition_NotEnoughCost` | below | 4404 | yes |
| P1:32551 | 1409; SP:50 | `Skill_InReaction` | at least | 1833 | yes |
| P1:35341 | 1423; SP:55 | `Skill_UnusableSkill` | at least | 60675 | yes |
| P2:8032 | 1001; none | `Skill_UnusableUtilityBag` | not applicable | — | no |
| P2:10059 | 1418; SP:10 | `Skill_InReaction` | at least | 933 | yes |
| P2:16051 | 1001; none | `Skill_UnusableUtilityBag` | not applicable | 327 | yes |
| P2:21562 | 1409; SP:50 | `Skill_InReaction` | at least | 1877 | yes |

**Observed error groups:** nine 1418 refusals report `Skill_Condition_NotEnoughCost` with latest observed SP below catalog SP:10; five report `Skill_InReaction` (1409×3, 1418×1, 1445×1); five report `Skill_UnusableUtilityBag` (item skill 1001); one each reports `Skill_InCutScene` (1100517) and `Skill_UnusableSkill` (1423). All are explicit server responses, not a client-local precheck. The 1423 response is not proof that the separately reported live-client attack refusal was sent to the server.

**Candidate causes:** The `Skill_InReaction`, `Skill_InCutScene`, `Skill_UnusableSkill`, and utility-bag names imply state checks, but the packets do not establish which specific stance/action/trait/equipment or item caused them. Target references do not prove legal range. Nine cost errors agree with an insufficient-resource interpretation, but a contemporaneous authoritative resource snapshot was not decoded. A zero catalog cooldown does not prove eligibility during a reaction. Do not replace these results with cooldown errors or a guessed numeric `ec`.

## Unclassified request and testable server contract

- **Observed:** P2 decoded line 30216 requests skill 1420 (catalog LongBow, SP:15, cooldown 0), 24,652 ms after its previous accepted use. No matched **13** or **14** follows before the capture ends (30,219 decoded records). Outcome unknown; not a 22nd rejection, not a proven acceptance or cooldown violation.
- **Server contract supported by observations:** For an owned, currently eligible weapon skill with no conflicting action and sufficient *authoritative* SP, accept the start, deduct its catalog SP cost once, and emit opcode **53** with stat-type-72 delta and a cooldown deadline of `server_tick + catalog.cooldown * 1,000,000` (wire ticks); accepted start opcode **14** is observed separately. When authoritative SP is insufficient for 1418 (cost 10), reject with `Skill_Condition_NotEnoughCost` via opcode **13** and do not deduct the cost. A deterministic test should set SP to 9 then 10, use the same valid weapon/action state, and assert the first response is error 13 and the second accepted path charges 10 once. The capture does not prove error precedence if other gates also fail.
- **Other test cases, conditional on independently established states:** If a request actually occurs while the character is in a cutscene, verify opcode 13 `Skill_InCutScene`; while reaction-bound, verify `Skill_InReaction`; when utility-bag eligibility is absent for 1001, verify `Skill_UnusableUtilityBag`. These are **Candidate predicates** attached to **Observed result names**, not permission to invent how those states are determined. For 1423, `Skill_UnusableSkill` is observed but its predicate is unknown; do not disable an otherwise permitted starting skill by guessing it. No explicit cooldown-refusal result or range/refusal ordering was observed among these 21 failures.

**Remaining evidence:** Confirm stat-type-72 enum name and exact rejection precedence from the matching build, and obtain original-client attack acceptance. The existing IDA GUI is intermittent; this capture/catalog-only task made no IDA calls or annotations.

## Mapping to `CombatSession::start` at `eb0834b` (`src/world/features/combat/combat.cpp`)

**Static** (source inspection, no run). Each row names the ec the server emits today for the observed condition. "Modelled" means a server state/field exists; it does not mean the predicate matches the official one.

| Observed ec (count) | Server today | Status |
| --- | --- | --- |
| `Skill_Condition_NotEnoughCost` (9, skill 1418, SP below SP:10) | Line 456-457: SP shortage emits `Skill_Condition_NotEnoughSP` (same `require` also covers HP cost). | **Modelled, name differs.** Observed ec is `NotEnoughCost`; `combat-refusal-mapping.patch` splits the check so SP shortage emits `NotEnoughCost`. HP shortage keeps its old code: `Skill_Condition_NotEnoughHP` exists in the descriptor but no HP refusal was captured (Candidate). |
| `Skill_InReaction` (5; 1409, 1418, 1445) | No server state: `combat.cpp` has no hit-reaction/stun/recovery-lock field (`recoveries_` is HP recovery). Line 453 `may_cancel` emits `Skill_UsingSkill` for an active action. | **Missing.** Whether the official reaction is a hit reaction, a dodge/cancel window or a combo gate is Candidate; do not map it onto `Skill_UsingSkill`. |
| `Skill_UnusableUtilityBag` (5; item skill 1001) | Item skills with `use_item` bypass `CombatSession` (`handles`, line 226-227) and go to `tutorial.cpp:953-1017`, which emits `Skill_UnusableItem/InvalidUseItem/NotFoundUseItem` and has no bag predicate. | **Missing.** Predicate unknown. |
| `Skill_InCutScene` (1; 1100517) | No cutscene state in combat or native_world gameplay path. | **Missing.** |
| `Skill_UnusableSkill` (1; 1423) | No equivalent; closest unrelated gates are `Skill_InvalidCmsId` (lines 397-402). | **Missing**, predicate unknown. |
| Cooldown (0 official refusals) | Line 413-414 emits `Skill_Cooldown` (exists in the descriptor). | **Modelled; official refusal never observed.** Server ec for cooldown is Candidate, not evidence-backed. |

No other ec in the 21 observed refusals has a server path. Check order in the server (`InvalidCmsId`, `UsingSkill`, `TooFar`, `Cooldown`, `InvalidTarget`, cancel gate, cost) is not established as official order; observed refusals never had two candidate causes at once, so no reorder is justified.

## The single `Skill_UnusableSkill` refusal (skill 1423) and “Cannot use this skill”

**Observed context (sanitized, P1 decoded line 35341):**

- Character class `EX5CharacterClass_Wild`; request `equip_slot=EX5EquipSlotType_Weapon_2`, lock-on target present, response `weapon_state_type=EX5WeaponStateType_Drawn`. Catalog skill 1423: `weaponType=LongBow`, `skillType=Command`, `isBasicAttack=True`, `SP:55`, cooldown 0, `weaponStateType=Drawn`, comment “Wild: longbow: basic attack: back input: 4th hit – retreat shot”. The same class/slot/skill was accepted 53 times in the three captures, so class, weapon type and drawn state were **not** the discriminator for this refusal.
- SP just before: 427 (cost 55), so cost was not the cause. The response arrived 22 ms after the request; the client had sent a root-motion request (20033) 2 ms after the start request and the server then answered `MoveRollback` (`Move_InvalidRequest`).
- Preceding accepted skills (ms before the request): 1434 (1683), 1463 (906), 1445 (829); 1445's action ended (`MoveCancelable`) 4 ms before the request. Status effect 2001005 had been removed just before it. By contrast, 29 of the 53 accepted 1423 requests followed 1420 and most followed a 1418→1420 chain 500–1000 ms earlier. Status 2001005 was active in 17 of 53 accepted requests, so it does not discriminate.

**Candidate:** the official server refused this 1423 request because the “4th hit” command skill was requested outside the 1418→1420 basic-attack chain. The decoded packets do not state the predicate; combo/chain data and server-side action history were not captured. Not an implemented rule.

**Comparison with `CombatSession::start` at `eb0834b`:** class mismatch (`combat.cpp:399-401`) and weapon mismatch (`combat.cpp:402`) both emit `Skill_InvalidCmsId`. With no active action (`action_` empty) the cancel gate at 434-453 permits any skill, so a command skill requested after an idle gap is accepted; no `Skill_UnusableSkill` path exists.

**Does the evidence prove the official server answers class/weapon unavailability with `Skill_UnusableSkill`? No.** The only observed `Skill_UnusableSkill` has matching class, weapon and state, and no capture contains a request that mismatches class/weapon, so the official ec for that condition is unobserved. `Skill_InvalidCmsId` for class/weapon mismatch is likewise **not** evidence-backed; it is existing server behavior. Because the proof is absent, no `combat-refusal-mapping-v2.patch` was written; the mapping stays Candidate.

**Which ec/precheck yields the displayed text (saved findings, `docs/combat-evidence.md`, journal under `dumps/ida_research/combat-20261024/`):** `L10N.csv` maps `TXT_ERRORCODE_0000005121_MESSAGE` to “Cannot use this skill.”; ec 5121 is `Skill_UnusableSkill`. Two paths can display it: a server `StartSkillFailRes` 13 (callback `0x1461E2E80` → display `0x145CCE2F0`, **static_verified**) and the client-local eligibility function `sub_14623F3E0`, which returns 5121 at `0x14623f4b0` when `sub_146044C80` finds the skill in neither the perk-gated list nor the default-unlocked set (**static** analysis, Candidate names, no request is sent). That perk list is populated by `PS2C_WorldCharPerkNoti` 135, so a missing/empty 135 for the character's weapon is the leading candidate for the live “Cannot use this skill” with zero server requests. The weapon-not-drawn gate returns a different code (5164). No new IDA query was run; the pictured live attempt has no correlated packet. *This hypothesis is refuted by the two sections below: the emulator sends a complete 135, and the gate semantics are corrected there.*

## World-entry `PS2C_WorldCharPerkNoti` (135): emulator versus official

**Static (source at `eb0834b`, same code at live baseline `46839f1`):** `native_world.cpp:2002-2006` encodes opcode 135 from `CharacterStats::starting_perks(class_id)` (`character_stats.cpp:488-501`) at the first time-sync of a connection (`native_world.cpp:1978`), after items (130) and before the map-move start. The condition is `character_stats_` loaded and a bootstrap character present. The packet is populated for all four classes: every `PerkNode` row with `autoRegistration=True` and the class name, sorted ascending, `level=1`, plus `remaining_char_build_points {1:0, 2:0}` (`character_stats.cpp:184-210`). Catalog counts: Fight 46, Wild 38, Adamant 32, Magic 42. A missing catalog throws at load; an absent perk set for a class throws at send, so an empty packet cannot be sent.

**Observed (official):** capture 1 (Wild) carries one 135 at world entry with 38 `perks` entries `{cms_id, level=1}` and `remaining_char_build_points {1:0, 2:0}`; an older official Magic entry carries 42. In both, the set of `cms_id` equals the catalog `autoRegistration` set for the class exactly (no extras, none missing). The official order is not ascending, which the emulator does not reproduce; whether the client depends on order is unobserved (Candidate: it does not, since the perk manager is a hash map). Captures 2 and 3 begin mid-session and contain no 135. Fight and Adamant have no official capture; their sets are Static, catalog-derived only.

**Static link to skills:** `PerkNode.jsonCase=SkillOpen` rows list `skillIdNN` values; `autoRegistration` nodes open the starting skills, for example Wild 1230107 opens 1423, Wild 1230103 opens 1418/1420/1421, Magic 1425102 opens 2220 (the staff basic attack), and Wild 1211802 opens 1100000. So the 135 catalog set does carry the starting-attack skill perks.

**Observed in live log (not correlated to the pictured attempt):** `out/local-postgres/server.log` records `Native world response write ... opcode=135 bytes=384` at connection start of two connections on 2026-10-24.

**Conclusion:** the premise “the emulator sends no/empty 135” is refuted for source at `46839f1`/`eb0834b`; no `world-perk-noti.patch` is justified. The remaining missing link for the client-local 5121 is therefore *not* absence of the notification but one of: (a) the client check also needs state that 135 does not provide (second check `sub_145DC3580`, the perk-type byte at +9, or `*(actor+376/377)` flags), (b) 135 is sent before the client creates the perk manager, or (c) the client check is for a different skill than the staff basic attack. Resolving this needs a matching-build trace of what `sub_146044C80` reads after a 135 is applied, or a paired client trial. This slice made no IDA call.

## Client perk gate: what the two bytes and the 135 fields do (static, IDA y12r)

Build: IDB `ArcheAgeChronicles.memdump.exe` SHA-256 `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`, image base `0x140000000`, build id `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. Names below were saved as `AAC_*_Candidate_*` with a journal entry (`dumps/ida_research/combat-20261024/annotation-journal.jsonl`) and `idb_save` returned ok. All findings are **Static**, not observed at runtime.

**Correction to the earlier summary.** `sub_146044C80(actor, &skillId)` is true unless the skill is *perk-gated and the gate fails*:

1. *Direct branch:* a table keyed by skill id lists perk records. A record counts only if its type is 3 and the byte at record `+9` is 0 or equals `sub_146057B80()` (actor byte at `+144` or an override). If its key int at `+12` has a non-zero level in the owned-perk map (`sub_145F26BE0` → `sub_145F26C30`, int-key hash lookup), the skill is usable.
2. *Requirement branch:* `sub_145DC3580(skillMgr, &skillId)` looks the skill up in a hash at skill-manager `+1696`; null (not found, or in the exempt sets at `+1776`/`+2016`) means **not gated, usable**. A record is a list of groups of `(byte, byte)` pairs; the skill is usable when every pair of one group passes `sub_145F29120(perkManager, &pair)`. If `sub_1460442B0()` returns null the pair test fails.
3. A skill found by the direct branch whose perk is not owned, with no passing requirement group, returns 0 and the caller returns **5121**.

**What `sub_145F29120` compares.** `pair[0]` selects an entry in a hash at perk-manager `+296` that holds the perk ids the character owns for that group; `pair[1]` must equal the byte at `+112` of one of those perk records (record type 8). The meaning of `pair[0]` (group key) and of that record byte is **not established**: `EX5PerkNodeThemeType`, `EX5PerkNodeCategory` and `EX5PerkNodeLayerType` all exist in the client, but no code reading them here was traced.

**What 135 feeds.** `sub_145F28CB0` reads `perks[]` (perk id at `+24`, **level at `+28`**) into an id→level map, and `remaining_char_build_points[key]` into two booleans: `actor+377 = (value != 0)` for key 1, else `actor+376 = (value != 0)` (so the earlier “flags at +376/+377” are build-point-remaining flags, both false for the emulator's `{1:0, 2:0}` and for the official Wild/Magic captures), then `sub_145F238E0` derives perk state. Level 1 is sent by the emulator and by the official captures.

**Catalog link (Static).** The direct branch matches the catalog `PerkNode` `SkillOpen` rows: Magic 2220 ← node 1425102 (`autoRegistration`, theme ElementalStaff); Wild 1418/1420/1421 ← node 1230103 and Wild 1423 ← node 1230107 (both `autoRegistration`, theme LongBow). All are in the 135 the emulator sends, at level 1, so the direct branch is satisfied for these skills **provided** the record `+9` type byte is 0 or equals the actor byte. The `+9` byte has no counterpart column in the extracted `PerkNode.jsonl` (no field maps to it) and was not decoded. The source table of the requirement branch (`+1696`) was **not** identified (no catalog table lists skills 2220/1418/1423 outside `PerkNode`, `Skill`, `SkillChanger`, `ChangeWeaponSkillSlot`, `WeaponSkills`).

**Server gap decision: none proven.** Nothing shows that the server's 135 lacks a level, slot or type that the client compares; level and id are sent, and the two build-point flags are false in the official captures too. No patch is written. **Remaining unknowns for the live trace:** (a) the actor byte at `+144` versus the perk record `+9` byte for the Magic staff character; (b) the requirement records and pair bytes for the pictured skill (the trace script logs `perk-requirement-lookup` and `perk-list-contains` pair bytes); (c) whether the perk manager exists when the first attack is tried.
