# Monster corpse loot

Status: **partial**. Server-side loot is implemented for the private tutorial instance and
covered by synthetic tests. No original-client trial of the loot flow has been observed against
this emulator. Part of the behavior is **CUSTOM**: our own guess, not a 1:1 reproduction of the
official server, because the data to reproduce it does not exist. Every custom rule can be
switched off; see [Custom / guessed behavior](#custom--guessed-behavior). The maintained status
and packet table live in `protocol/feature-coverage.json` (feature `loot`).

## Evidence used

* The three 2026-10-24 official captures contain 77 `PC2S_LootItemReq` requests. The decoded flow
  (private `out/captures/`, not published) is:
  1. At the kill, the server sends `PS2C_UnitLootItemInfoNoti{has_chance_to_loot}` and a
     gold-only `ItemsUpdateNoti` (reason `looting`), then the Demise state change, then
     `PS2C_DeathNoti`.
  2. The client sends `PC2S_LootItemReq{unit_id}` about one second later.
  3. The server answers with `PS2C_ItemsUpdateNoti` (reason `looting{target_unit_id}`, carrying
     `items`, `stackables` and `money_changes`, an empty `packet_seq`), a
     `PS2C_UnitLootItemInfoNoti` with both flags clear, and `PS2C_LootItemRes`. When the roll
     produced nothing, only the last two packets arrive.
  4. `PS2C_UnitRemoveNoti` removes the corpse about 10 s after it was emptied.
  5. Rejections use `PS2C_LootItemRes.ec`; `Action_InvalidLootingRange` occurs 10 times.
     `ItemsUpdateNoti` stack entries report the owner's resulting total.
* All 63 captured loot grants that could be joined to a killed NPC contained only items from
  that NPC's `Npc.FK_lootPackId` pack.
* `GameVariable.LootingRange` (500, centimetres), `NpcCorpseLootingDestroyMillis` (90 000) and
  `NpcCorpseNoneDestroyMillis` (10 000) provide the range and the two corpse lifetimes. The
  captured removal delay matches the second.
* Packet fields come from `protocol/descriptors.normalized.pb`; error names from
  `x5.ec.EErrorCode` (`Npc_InvalidId`, `Npc_ImpossibleLoot`, `Npc_HasNoLoot`,
  `Npc_LootOverMaxWeight`, `Npc_FailLootTransaction`, `Action_InvalidLootingRange`).

## Data pipeline

`python tools/build_loot_data.py` writes `data/generated/loot/definitions.json` (format version 2,
ignored build output) from the verified catalog export. Tests: `python -m unittest
test_build_loot_data` in `tools/`.

* `Npc.FK_lootPackId` (monsters only) selects a `LootPack`. Its `LootCondition` rows are
  independent groups; each group's `LootItem` rows are weighted alternatives with a quantity
  range. Item storage follows the NPC-shop split: `Equipment` becomes an item instance
  (catalog grade and durability); `Consumable`, `CraftMaterial`, `PolishingMaterial`,
  `Repairer` and `Etc` become one stack per CMS ID; authored `Gold` rows become money.
* Groups that depend on a custom rule are **emitted and tagged** with the rule's name
  (`custom_rules`). The server withholds a tagged group when that rule is off, so switching a
  rule off restores the evidence-only behavior without rebuilding the profile.
* A group is still skipped as a whole, and listed under `skipped_groups`, when a part is not
  covered by any rule: `contributionType`, `publicAble`, `worldLevel` or `FK_PlayRecord_Start`
  filters, other currencies, a grade on a row, unknown items, `WayPoint`, `Memo` and
  `CraftRecipe` items, or an equipment row granting more than 16 units. Skipping the group,
  rather than one alternative, keeps the remaining weights correct. The current export emits 513
  loot packs, 8 handling packs and 310 items for 3,215 monster NPCs and skips 25 groups.

## Runtime (`src/world/features/loot/loot.*`, `src/world/features/loot/loot_rules.*`)

* `LootData` validates and indexes the profile once at startup and applies the custom rules. A
  profile for another client build, another format version or with inconsistent references
  fails the server start.
* `LootRandom` supplies every random decision (`SeededLootRandom` in production, scripted in
  tests). `roll_loot` draws in a fixed order: pack rate, then for each group its rate, one
  weighted entry and a quantity.
* **Rolling and ownership.** `CombatSession` calls a defeat observer when a monster's health
  reaches zero, before the Demise and death packets. `LootSession::monster_defeated` records a
  corpse owned by the killing character and rolls its contents once. A corpse for an NPC with
  loot facts announces `has_chance_to_loot`, even if the roll is empty, as in the captures. The
  tutorial instance is private, so the owner is always its single player; the owner check still
  rejects a corpse credited to another character.
* **Taking.** `PC2S_LootItemReq` is answered with exactly one `PS2C_LootItemRes`. The request
  must name a tracked, unexpired corpse the character may take, within `LootingRange` plus the
  tolerance, with loot not yet taken. The plan then adds gold and stack quantities (bounded by
  the Gold limit, `int`, and 1,000 stack kinds) and one inventory row per instance in free slots.
  An optional weight policy hook can refuse the take; production leaves it unset because no
  weight rule is evidenced. Created rows carry `created_epochtime` and, for custom polished
  drops, `polish_step`.
* **Atomicity and retries.** Progress and new item rows commit together through
  `AuthService::commit_world_item_trade` under the character's progress revision. The tutorial
  session adopts the committed revision (`tutorial_loot_persistence`) so its later writes do
  not conflict. A refused or failed take writes nothing and leaves the corpse lootable. A
  successful take marks the corpse empty, so a repeated request is refused and cannot grant
  twice.
* **Expiry.** A corpse with loot lives `NpcCorpseLootingDestroyMillis` from the kill (captured:
  two unlooted flagged corpses were removed 90.04 s and 90.07 s after death); after a take it
  lives `NpcCorpseNoneDestroyMillis` more (never extending the earlier deadline; captured
  removals came about 11.1 s after death, one second after the auto-loot request). The world
  session's gameplay timer calls `LootSession::advance`, which sends `PS2C_UnitRemoveNoti`.
  The defeat observer returns `manages_corpse_removal`, so combat's own default timer
  (`kCorpseRemovalDelayMs`, Candidate) does not remove the same unit a second time. Monsters
  without loot facts keep combat's default removal. A session tracks at most 1,024 corpses.

## Custom / guessed behavior

Everything in this section is **CUSTOM (guessed, not official)**. It is on by default for local
play and every item can be switched off. In code, each guess carries a
`CUSTOM (guessed, not official)` comment or lives in `LootCustomRules`
(`src/world/features/loot/loot_rules.hpp`); in the generated profile the dependent groups carry the rule name.

### Configuration

* `--loot-custom-rules on|off` (flat `--config` key `loot_custom_rules`): `off` selects
  `LootCustomRules::evidence_only()`: every rule below off and a zero position tolerance.
* `--loot-rules-file PATH`: a JSON file `{"custom_rules": {...}}` with the keys below. Missing
  keys keep their defaults; unknown keys, wrong types and out-of-range values stop the server at
  startup. Ignored when `--loot-custom-rules off` is given.
* At startup the server logs one line, `Loot custom rules (guessed, not official): ...`, naming
  the active rules and every guessed detail, plus the number of groups the rules withhold.

| Key | Default | Meaning |
| --- | --- | --- |
| `kill_gold` | `true` | Pay fitted kill gold with the corpse. |
| `first_kill_rewards` | `true` | Allow first-kill groups. |
| `quest_gated_rows` | `true` | Allow quest-gated items and `QuestItem` rows. |
| `full_moon_stones` | `true` | Allow `FullMoonStone` rows. |
| `polish_step_rows` | `true` | Allow `polishStep` equipment rows. |
| `handling_loot` | `true` | Roll the handling pack. |
| `party_shared_loot` | `true` | Let nearby party members share a corpse. |
| `range_tolerance_mm` | `2000` | Slack added to `LootingRange`. |
| `party_range_mm` | `30000` | Party sharing distance from the kill. |
| `repeated_take_error` | `Npc_HasNoLoot` | Refusal for a corpse already taken or without loot. |
| `full_bag_error` | `ItemSlotLimit` | Refusal when no inventory slot or stack kind is free. |
| `count_limit_error` | `ItemCountError` | Refusal when a stack or gold total would overflow. |
| `over_weight_error` | `Npc_LootOverMaxWeight` | Refusal from the optional weight policy. |
| `empty_roll_reply` | `success` | `success` or `no_loot_error` when the roll left nothing. |
| `lootable_lifetime_ms` | catalog (90 000) | Lifetime of a corpse with loot, from the death. |
| `emptied_lifetime_ms` | catalog (10 000) | Lifetime after a take or for a corpse without loot. |
| `emptied_lifetime_anchor` | `take` | `take` or `death`: where the emptied lifetime starts. |

### Rules

**Kill gold (`kill_gold`).** *Guess:* every monster pays gold with its corpse, from a straight
line per NPC template grade fitted to the captured samples,
`gold = round(base + per_level * level)` (at least 1). Boss grades `Boss_A`, `Boss_C` and
`Boss_S` reuse the `Boss_B` curve; NPCs whose template grade was never sampled pay nothing.
*Evidence:* captured corpse gold (including for NPCs with no loot pack) was the same for every
repeated kill of the same NPC, so the formula has no random range. The 23 distinct
grade/level samples are `GOLD_SAMPLES` in `tools/build_loot_data.py`; the fit reproduces
Weak, Normal and Strong exactly, Elite within 1 gold and Boss within 6:

| Grade | Samples (level: gold) | Fit |
| --- | --- | --- |
| Weak | 4, 6, 7, 8: 3 | 3.0 + 0.000 × level |
| Normal | 2: 6, 4: 6, 5: 7, 6: 7, 7: 8, 8: 8 | 4.94 + 0.386 × level |
| Strong | 2: 9, 5: 11, 8: 13 | 7.67 + 0.667 × level |
| Elite | 2: 90, 4: 102, 6: 115, 8: 129 | 76.5 + 6.5 × level |
| Boss_B | 2: 338, 5: 405, 6: 430, 7: 457, 8: 485, 9: 516 | 282.6 + 25.28 × level |

Levels beyond 9 are extrapolation. *Timing differs from the official server:* the captures show
gold granted at the kill, before any request; the emulator grants it with the corpse take, as
requested, because the death observer must not touch storage. Gold goes through the same commit
and Gold-limit check as the rest of the take.

**First-kill groups (`first_kill_rewards`).** *Guess:* groups filtered by `lootingClass` or
`FK_PlayRecord_End` are first-kill rewards. They drop only on a character's first looted kill of
that NPC; taking the corpse records the NPC in `WorldProgress::loot_first_kills`, persisted
with the corpse take in table `emulator_character_loot_first_kills` (schema version 22,
`migrations/001_accounts_characters.sql`; a migration was needed because no existing table
fits a never-removed per-NPC set). Another corpse of the same NPC waiting to be looted does not
also get the group, and the reservation ends when its corpse is taken or expires. The first kill
is spent whether or not the group's rate roll succeeded, and is evaluated for the killer but
recorded on whoever takes a shared corpse. *Evidence:* none for the official trigger. The
`PlayRecord` tables were not modelled.

**Quest-gated rows (`quest_gated_rows`).** *Guess:* an item with `Item.questLooting` is an
alternative in its group only while the character has one of the listed quests active (present
and not complete in `WorldProgress::quests`). `QuestItem` rows join this rule. *Fallback:* most
listed quest IDs (for example 20002048) are not defined in the demo client's Quest table, so
they can never be active; items that list no defined quest drop without a gate. *Evidence:* the
column is a list of quest IDs on 28 items; no capture shows a gated drop.

**Full moon stones (`full_moon_stones`).** *Guess:* `FullMoonStone` items drop as ordinary
stackable items, one stack per CMS ID. *Evidence:* none; no captured loot contained one.

**Polish-step rows (`polish_step_rows`).** *Guess:* `LootItem.polishStep` is the polish step of
each dropped equipment instance (`CharacterItem::polish_step`). *Evidence:* the 82 rows are all
equipment and the step range (2 to 12) fits the item polish steps; no capture shows one.

**Handling loot (`handling_loot`).** *Guess:* `Npc.FK_handlingLootPackId` is a second pack. When
a roll with probability `Npc.handlingRate` passes (an empty rate means always), its contents
join the corpse and are taken with the ordinary `PC2S_LootItemReq` (the corpse then announces
`has_chance_to_loot`). An NPC with only a handling pack is lootable only when the roll passes.
*Evidence:* none for this column. The captured `possible_handling_loot` flag belongs to the
evidenced knife butchering below, so since 2026-10-30 this guessed pack no longer sets it.

**Party shared loot (`party_shared_loot`).** *Guess:* connected members of the killer's party in
the same instance and map, within `party_range_mm` of the kill, may take the corpse. Each gets
`has_chance_to_loot` when the corpse is created. The corpse is taken **once, as a whole, by the
first eligible member to claim it** (`SharedLootBoard::claim`, process-wide and atomic); the
others are told with a cleared flag and are refused afterwards. A failed commit releases the
claim. The owner's corpse timer governs the shared corpse. *Limitation:* party membership comes
from `PartyWorld::nearby_party_members`, but monsters exist only in each character's private
tutorial instance, which only that character shares, so in production the audience is always
empty until shared-world monsters exist. The policy is exercised by synthetic tests only.

### Guessed details

Each is a named constant or configuration value (table above):

* **Range tolerance (2 m).** `kGuessedRangeToleranceMm`. The server knows the player's position
  only from the last report; a rough position analysis of the captures put refusals at 4.8 m and
  farther and successes up to about 19 m, so the official rule is not a plain distance and 2 m is
  a pure guess.
* **Repeated take.** `Npc_HasNoLoot`. The captures contain no repeated take.
* **Full bag.** `ItemSlotLimit` for no free slot or stack kind, `ItemCountError` for overflow,
  `Npc_LootOverMaxWeight` for the weight policy. No capture shows any capacity refusal.
* **Nothing dropped.** A successful reply with cleared flags; the captures show a successful
  reply with no items update for corpses that dropped nothing. The alternative is
  `Npc_HasNoLoot`.
* **Timer anchors.** A corpse with loot lives 90 s from the death; an emptied corpse 10 s from
  the take, never beyond its earlier deadline. The captures fit both an anchor on the take (10 s
  after it) and one on the death (about 11 s after it, the auto-loot comes 1 s after the death),
  so `emptied_lifetime_anchor` selects either.

## Corpse butchering (evidenced, 2026-10-30)

The official capture `wavesword-mob-combat-20261026` (private `out/captures/`) contains three
complete butcherings. The 17 `possible_handling_loot` flags that earlier could not be joined to
`Npc.FK_handlingLootPackId` belong to this mechanism instead:

1. The monster's `PS2C_UnitAddNoti` nub carries `npc.is_handling_loot_candidate` (Alpha Wolf
   5001029). Its `Npc.FK_expertiseRewardId` (3104238) names an `ExpertiseReward` loot pack.
2. At the kill the killer receives `PS2C_UnitLootItemInfoNoti{possible_handling_loot}`.
3. The client sends `PC2S_StartInteractionAnimReq{interaction_anim_id 1000, param.handling_loot,
   target_unit_id = corpse}`; the server answers `PS2C_StartInteractionAnimNoti` echoing it.
4. 2.93 s later `PC2S_ExecuteInteractionReq` is answered by `PS2C_ItemsUpdateNoti` (system
   reason) with the knife row worn by 100 (1500 -> 1400) and exactly the pack's items (Wolf Molar
   at 100 %, Primordial Echo at 30 %; Wolf 5001030 gave Raw Pelt 1-3), then the corpse's cleared
   `UnitLootItemInfoNoti` and `PS2C_InteractionAnimRes`.
5. A butchered corpse was removed 10.1-10.8 s after the butchering; an unbutchered one 90.2 s
   after the death (`NpcCorpseNoneDestroyMillis` and `NpcCorpseLootingDestroyMillis`).

Implementation:

* `tools/build_gathering_data.py` exports `handling`: animation 1000 (the only Knife
  InteractionAnim; montage execute 2,930 ms, end 3,833 ms) and the strict ExpertiseReward pack
  of every monster (101 monsters; 2 are listed unsupported for an equipment item or a filtered
  group). `tools/build_world_mobs.py` marks those spawns `handling_loot_candidate`, and
  `combat_world.cpp` sends the nub flag.
* `LootSession` owns the rights: only the killer's unexpired, unbutchered corpse is butcherable.
  Such a corpse is announced and kept for the lootable lifetime even without corpse loot; an
  ordinary take keeps it (the take reply keeps `possible_handling_loot`), and `corpse_butchered`
  shortens it to the emptied lifetime.
* `GatheringSession` runs the interaction: knife equipped in a tool slot, SP, 7 m range, server
  execute deadline, one revisioned commit of the rolled stacks and the knife wear. It accepts
  butchering on every map, not only gathering areas.

Bone Chips (70108003) for *Gathering Bone Fragments* drop this way from the coastal alligator
and alligator turtle family (for example 5001023-5001026 on map 9011); no monster has them in
its corpse loot pack.

## Known limits

* Official kill gold is paid at the kill; the custom gold is paid with the take.
* Skipped groups (`contributionType`, `FK_PlayRecord_Start`, `WayPoint`, `Memo`, `CraftRecipe`
  rows and the like) never drop.
* There is no contribution or public-loot model, and monsters exist only in private tutorial
  combat, so party sharing is dormant in production.
* Weight limits are not enforced; item rows have no affixes or sockets.
* Only the killer may butcher a corpse; party members of a shared kill cannot. Butchering grants
  no expertise experience (the authored `ExpertiseReward` Exp rows have no captured effect).
* Butchering is covered by synthetic and real-profile TLS tests
  (`NativeWorld.KillerButchersAWorldMonsterCorpseWithAKnifeOverTls`); the original client has not
  yet butchered a corpse against this emulator.
* PostgreSQL commits use the already-shared `commit_item_trade` path and the new first-kill
  table; the loot tests run on the memory store and a live PostgreSQL loot run was not performed.
* The command-line options were exercised by starting the server, not by a unit test.
