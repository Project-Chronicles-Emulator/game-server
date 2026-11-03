# Tutorial quests and saved progress

Quest definitions come from the verified catalog and map layers through
`tools/tutorial_quests.py`. The primary profile contains all seven quests in
map 8000. Its separate map 10117 followup profile contains quest 10000046 and
hidden quest 90100011. Profiles scope NPC instance IDs and trigger geometry to
their own maps; matching an ID in another map does not authorize an action.

The sequential chain is recovered from `Quest.sequentialGroupOrder`, rather
than maintained as a separate hardcoded order. Objective groups follow the
captured semantics: alternatives within one group use OR, all groups must be
satisfied, and sequential quests require earlier groups before later ones.

## Objective sources

| Objective | Authority and validation |
| --- | --- |
| Trigger entry | Catalog GUID, correct request kind, session map and position, volume geometry, objective order |
| Trigger exit | Same checks, with position near the volume boundary; a request from the center is rejected |
| Dialogue selection | Active local dialogue target, NPC CMS ID, dialogue and selection orders, position and range |
| Doodad interaction | Validated server interaction execution; clients cannot claim it through a progress packet |
| Hunt | `TutorialSession::record_monster_defeat`, called by authoritative combat after a local monster dies |

The hunt API resolves the monster's CMS ID from the current instance. It
increments only matching active objectives, caps their counts, and records
the credited local monster ID. It evaluates eligibility before the event, so
one death cannot satisfy a newly unlocked sequential group or a newly accepted
successor quest. The combat caller must establish the death and instance
ownership; the quest API does not interpret a client's target list as a death.

Quest reward requests validate all objective groups. They commit the completed
flag, success count, item or gold awards, experience, and sequential successor
together. Experience is cumulative and levels the character up through
`src/world/features/character/progression.hpp`, granting LevelUpPoints. Quest CharBuildPoint rewards add
their build points, and combat-established kills add kill EXP in the same commit (see
[progression.md](progression.md)). Repeated acceptance does not duplicate supply items. Acceptance of
an already completed quest returns its completed state and does not restart it.
Hidden automatic quests complete only after their own objectives are satisfied.

## Persistence

Migration version 6 adds `emulator_character_quests.credited_kills` with an empty
array default. Existing quest flags, counters, inventory, object phases, and
characters remain in place. The normal startup migration applies this additive
change before the updated server uses it.

Kill counters and receipts use the existing atomic character-revision transaction.
No success notification is returned before the commit succeeds. A failed commit
leaves the session's counters and receipts unchanged. A reconnect loads the same
receipts, so a duplicate server death event cannot add another count. Storage
also rejects removal of a receipt, a decreasing objective counter, or resetting
a completed quest, unless the reset comes with a higher `attempt` (abandonment
or a repeated job).

Schema version 37 is additive: `emulator_character_quests` gains `attempt`
(default 0), `abandoned` (default false) and `started_epoch_ms` (default 0), and
two tables store job board periods (`emulator_character_job_boards`) and play
record counters (`emulator_character_play_records`). Existing quest rows keep
their progress unchanged.

Quest progress is independent of movement checkpoints. Saving objectives does
not establish authoritative movement, collision, damage formulas, or NPC AI.

## Shared-world quests (all client quests)

`tools/build_quest_catalog.py` normalizes every quest in the verified client export into
`data/generated/quests/catalog.json` (1,571 quests; rows that repeat per class are merged).
Each entry keeps the CMS acceptance rules, `QuestCondition` rows, objective groups, placed
trigger geometry tagged with its world asset, fixed rewards, supplies, the sequential successor
and an explicit `unsupported` list. `tools/start_persistent.ps1` and the client export rebuild
it; the world server loads it beside the client catalog.

On every map outside the private tutorial instance, `WorldQuestBook` (`src/world/features/quests/world_quests.cpp`)
handles acceptance, progress, rewards and NPC dialogue for catalog quests:

| Rule | Server evidence |
| --- | --- |
| Accept from NPC | NPC announced to this session, its template, interaction range |
| Accept from volume | Tracked position inside the placed volume of the same world asset |
| Accept after a quest / level | Saved completion or character level |
| Sequential successor | Accepted by the server when the predecessor completes |
| Conditions | `Quest`, `CheckUnitLevel`, `Company`, `OwnItem`, `PlayRecord` and `AdventureObject`, grouped AND/OR |
| Trigger enter / leave, dialogue selection, `InteractNpc` | Same checks as the tutorial |
| `InteractNpc` report | `ProgressQuestReq.interact_npc` names an announced NPC of the template in range; it confirms the dialogue credit |
| `InteractMemo` | `ProgressQuestReq.interact_memo` names a placed memo spot object of the objective, in the current world, within 7 m |
| `HuntMonster` | Server-owned monster defeat events, with per-quest kill receipts |
| `CounterSuccessCrafting`, `CounterTryCrafting` | A crafting-station recipe committed by the processing service |
| `InteractDoodad` | A gathering, quest or portal doodad interaction committed by the shared doodad engine (doodad and phase) |
| `EtherealHuntMonster`, `EtherealInteractDoodad` | The same kill or doodad events, adding a virtual amount (Candidate, below) |
| `GetItem`, `CharLevelUp`, `EquipItem`, `AdventureObjectStatus` | Saved inventory, level, equipment and objects; re-checked at turn-in |
| NPC turn-in | One of the catalog's turn-in NPC templates, announced and in range |
| `GetItem` turn-in | The satisfied objectives' stacks are removed before the rewards |

NPC rules accept either a unit announced to this session or an exact client-owned static NPC
identity from the map-scoped static interaction index (`src/world/features/npc/map_static_npcs.cpp`). Many quest
NPCs on the Shining Shore channels are static, for example Herlei (1001600) for quest 10000134
"Way of the Hunt" on map 9012. Until 2026-10-27 the index only covered Release-package maps,
so those dialogues were refused and their `InteractNpc` objectives stayed at zero. See
[world-latency-20261027.md](world-latency-20261027.md).

Supplies, gold, fixed item packs and experience commit with the quest flag in one revisioned
write before any packet is published. Since 2026-10-30 all 1,571 catalog quests have no
unsupported requirement; see [Full catalog coverage](#full-catalog-coverage-2026-10-30) for
the mechanics added for job boards, abandonment, server-offered starts, doodads and the
remaining objective kinds. Dependent-item retrieval is still not implemented. Shared rules
(objective groups, sequential readiness, grouped conditions, trigger geometry, rewards) live in
`src/world/features/quests/quest_rules.cpp` and are used by both paths.

## Full catalog coverage (2026-10-30)

`QuestCatalog` loads `quests=1571 fully_supported=1571`. The regression
`QuestCatalog.EveryGeneratedQuestIsSupportedAndCompletesWithItsRewards` completes every quest of
the generated catalog with all of its rewards. Synthetic coverage of each mechanic is in
`src/world/features/quests/tests/all_quest_kinds_tests.cpp`. None of this has been played with the
original client yet.

### Starting quests

| Start | Server rule |
| --- | --- |
| Job board (338 quests) | `AcceptQuestReq.unit` names the board doodad (captured client flow `AAC_QuestJobBoard_SendAcceptForBoardUnit`), placed near the player, and the quest is one of the board's current offers |
| Doodad (`InteractDoodad`, 58) | `AcceptQuestReq.unit` names a placed doodad of the rule near the player, in the authored phase when the server tracks it; an interaction the shared doodad engine executes in that phase also starts it |
| Memo (`InteractMemo`, 23) | `AcceptQuestReq.unit` names a placed memo spot object of the rule near the player |
| Never-placed start, development quest (135, ids below 1,000,000) | CUSTOM `GmOnly`: only the GM quest command starts it |
| Never-placed start, live quest (31) | CUSTOM `ServerOffered`: the server starts it when the player talks to an objective NPC, defeats an objective monster or uses an objective doodad; without such an anchor, on a map of its journal zone (any shared map without one) once the preceding quest of its series is complete |

A start is "never placed" when its trigger volume, memo, doodad or quest-giver NPC appears in
no exported layer of this client build (125 development quests share the placeholder volume
`52a66f04-...-bf8c820bcd9b`; the placed volume ends in `...9a`). Layered volumes
(`mapLayer` = a dungeon or channel Map id) are now matched on their own map.

`requireGold` and `requireItemIds` are acceptance gates answered with
`Quest_IsNotAcceptable_RequireGold` and `Quest_IsNotAcceptable_RequireItemId`. They are not
consumed, because successive quests such as 30003121 and 30003123 require the same quest items.
Conditions add `OwnItem` (held count), `PlayRecord` (server kill counters per record in
`emulator_character_play_records`, or holdings for GetItem records) and `AdventureObject`
(Candidate: `active` asks whether the object is activated).

### Job boards

`PC2S_QuestJobBoardReq{quest_job_board_cms_id, doodad_unit_id, refresh_using_cost}` is answered
with `PS2C_QuestJobBoardRes{jobs: {board: quest_cms_ids}, condition_not_met_ids}`. The board has
`QuestJobBoardPickOnceCount` (3) slots. Each slot uses the highest `QuestJobBoardCategory` row the
character qualifies for (company and position level, or guild level for board 1000) and draws one
quest from its `QuestPickGroup` with a deterministic hash of the character, board, period and paid
refresh count (CUSTOM; the official draw is not captured). Periods follow
`QuestJobBoardRefreshSchedule*` (daily from 2022-09-01 00:00 UTC). A paid refresh costs
`QuestJobBoard.resetCost` Gold (`MoneyNotEnough` without it) and draws again. The first request of a
new period turns the board's completed jobs into abandoned tombstones and removes them from the
client with `PS2C_QuestAbandonRes{force: true}`, so they can be taken again. Jobs are turned in with
`QuestRewardReq.unit_id` naming the board (`AAC_Quest_SendQuestRewardReqForUnit`). GuildExp rewards
are credited to the character's guild through `GuildService::award_experience` after the commit.

### Abandonment and repetition

`PC2S_QuestAbandonReq` answers `PS2C_QuestAbandonRes{quest_cms_id, force: false}` for an accepted,
incomplete quest whose `isAbandonable` is set (`Quest_AbandonFailed` otherwise). Progress rows are
never deleted: an abandoned quest stays as a tombstone (`abandoned`) with zeroed counters and a
higher `attempt`, and the quest service treats it as not accepted. Accepting it again starts the
next attempt, keeps the success count and only tops supplies up to the supplied amount. Storage
accepts a reset of the completion flag, counters or kill receipts only together with a higher
attempt (schema version 37).

### Objectives

| Kind | Server rule |
| --- | --- |
| `HuntMonster` / `EtherealHuntMonster` naming `npcIds` or `npcGroupId` | A defeat of any listed template or group member (job board hunts list several templates) |
| `InteractDoodad` / `EtherealInteractDoodad` on quest doodads | The shared doodad engine (below) executes the interaction and reports the doodad and phase |
| `TimeElapsed` | Server clock since the attempt was accepted (`started_epoch_ms`), re-evaluated on time sync |
| `CheckHp` | The player's health ratio from combat at or below `hpRate`, re-evaluated on time sync |
| `CompanyContributionTotal`, `housingFunishing` | Company standing; any installed housing furniture |
| `CompanyContributionGain`, `LevelExpGain` | Contribution and EXP the server credits after acceptance (quest rewards and kills) |
| `AdventureLogbookPercentage` | CUSTOM: the client-computed logbook report for the zone, at or above the authored percentage |
| `CounterUseItem`, `UseItemInTriggerVolume` | A committed item use (item-use session, item skills); the second also needs the player inside the volume |
| `TradePayment` | A trade good sold at the named trading post (price ratio not checked) |
| `CounterUseSkill`, `CounterUseSkillEffect`, `AddStatusEffect` | Combat: an accepted skill start, an executed effect of the player's skill, a status newly applied to the player |
| Step whose target this client build never places (24) | CUSTOM `auto_credit`: credited once the step is ready (unplaced volumes and memos, monsters no spawner places, doodads never placed or without an interaction, NPCs never placed) |
| `DestroyObject` (test quest 9088 only) | CUSTOM `auto_credit`: destructible objects are not simulated |

A turn-in NPC that is never placed is dropped from the turn-in list; with none left the quest
completes automatically. Items that only a quest asks to use (waypoint stone 10140 for quest
20001004) are spent by `PC2S_ItemUseReq` and reported to the quest service.

### Rewards

Random `LootPack` rewards (16 quests) are rolled once at completion with the pack and group rates
and weighted items (Gold and Hersander token rows included). `Mate` rewards register the mate
(`PS2C_RegisterMateNpcRes`), `Intimate` raises a registered mate's intimacy up to its
`MateIntimateLevel` total (`PS2C_ChangedMateIntimateRes`), and `CharBuildPoint` rewards add
LevelUp or Weapon points. `GuildExp` is applied after the commit like company rewards. A
repeated job's completion counts again for company and guild rewards.

### Shared doodad engine

`tools/build_gathering_data.py` now also exports quest doodads (those quests accept, complete or
count interactions with) and portal doodads (an interaction whose `playerTask` is `MapMove`) with
`quest: true`. Interaction phases keep their animation; an interaction without one is `instant`
and is used with `PC2S_InteractUnitReq`, answered by `PS2C_InteractUnitRes`. Phases without a
trigger return to the initial phase after `Doodad.respawnDelayMin` (60 s without one), so a shared
doodad does not stay used for everyone (CUSTOM). A phase without a flow still offers its
`DoodadPhase.interactionGroupId` interaction. Executing a portal moves the player to the
interaction's map and position with the zone-channel travel path. Quest doodads of dungeon and
story worlds are placed per world (`worlds` index on maps and doodad points), because spot unit
ids repeat across worlds.

### World population fixes for quest targets

`tools/build_world_mobs.py` now reads dungeon runtime layers below `MazeDungeons/` (dungeon maps
such as 10111 had no monsters), spawns `NpcSpawner.npcGroup` spawners (`"group:weight"`, 75
spawners were silently empty) and treats `npcIds` lists and NPC group members as quest hunt
targets. Story test worlds show their Static layers on their field map.
`tools/build_stationary_npc_placements.py --all-maps` keeps roaming NPCs without a path (quest
giver Ruby 1001174) and NPCs with an unsupported patrol standing at their first point (CUSTOM).

## Objective audit (2026-10-30)

Every objective kind of the 1,571 catalog quests was checked against the client tables and the
official captures. The table lists what the extracted data and the captures establish.

| Kind (objectives) | Evidence | Server status |
| --- | --- | --- |
| `GetItem` (425) | Client derives the count from the inventory; turn-in removed exactly the required stack (10000096 bone chips 6 -> 1, 10000110, 10000125) | Implemented, with consumption |
| `HuntMonster` (212) | Server kill events | Implemented |
| `TriggerVolumeEnter/Leave` (180), `DialogueSelection` (162), `InteractNpc` (168) | Captured reports | Implemented |
| `InteractDoodad` (105) | Doodad phase graphs in the client data | Gathering, quest and portal doodads through the shared doodad engine (2026-10-30) |
| `AdventureObjectStatus` (45), `EquipItem` (18), `CharLevelUp` (3) | Saved state | Implemented |
| `InteractMemo` (21) | Captured `interact_memo{memo_cms_id, spot_object_id}`; memo spot objects in the map layers | Implemented (3 objectives name an unplaced memo) |
| `CounterSuccessCrafting` (16), `CounterTryCrafting` (1) | Captured QuestUpdateNoti after the craft result | Implemented |
| `EtherealHuntMonster` (9), `EtherealInteractDoodad` (12) | Authored `rate`, `minValue`, `maxValue`; no capture | Candidate |
| `TimeElapsed`, `CheckHp`, `LevelExpGain`, `CounterUseItem`, `CounterUseSkill(Effect)`, `AddStatusEffect`, `DestroyObject`, `UseItemInTriggerVolume`, `TradePayment`, housing, company, logbook | No capture of the report or of the server rule | Implemented 2026-10-30 as server rules (see Full catalog coverage); Candidate |

Acceptance by reading a memo (28 quests) or using a doodad (72) uses `AcceptQuestReq.unit`
naming the placed object (recovered client flow, 2026-10-30; see Full catalog coverage).

**Crafting and doodad events.** The processing and gathering services record a `QuestEvent`
(`src/world/core/quest_events.hpp`) only after their own commit. The world session hands the
events to `QuestsSession::record_events`, which credits ready objectives in a separate
revisioned commit (planned again once on a revision conflict) and appends the QuestUpdateNoti
after the producer's reply. A craft request's count is the credited amount; a doodad objective
with `targetPhase` needs the interaction to run in that phase.

**Memo boards.** `tools/build_quest_catalog.py` exports every placed memo spot object
(`spot_object.json` `memoObject`) as `memos: [{world, unit_id, center}]` on InteractMemo
objectives. The captured Words of Warning report named spot `20337237082255366` (memo
10000127, Zone_01_Public_02), 2.1 m from the reporting position.

**Ethereal objectives (Candidate).** With `rate` percent chance a matching kill or doodad
interaction adds a uniform amount between `minValue` and `maxValue`, capped at the count. The
field names are authored; the official roll is not captured.

**Turn-in consumption.** Only stacks of satisfied GetItem objectives are removed; utility-bag
reservations stay, and item instances (equipment, official QuestItem rows) are not removed
because the emulator holds quest items as stacks. The update carries
`quest_reward{quest_cms_id}` and the remaining totals, as captured.

### Quest-gated world items (2026-10-30)

Some quests ask the player to take an item from a placed object. First Hunt (10000125,
Herlei, map 9012) starts with "Obtain Hunting Knife": objective group 1 is
`AdventureObjectStatus{20019119, Activated}`. Adventure object 20019119 is a `LootingObject`
whose interaction 10000135 has `FK_conditionId` 10000125 (`InteractionCondition`: quest
10000125, progress group 1, `Progressing`) and an ExpertiseReward loot pack 3101411 that always
yields the Hunting Knife 11200301. Because the shared adventure exporter skipped every
conditional interaction, the object was absent from the profile and the client's use request
went unanswered, so pressing the interaction key did nothing.

`tools/build_adventure_objects.py` now also exports activating `Use` interactions whose
condition rows are all `Quest` or `OwnItem` kinds, normalized by the quest catalog's
`quest_conditions` into `interaction_conditions`. 27 placements were added (all 1,933 previous
placements unchanged), for example the knife 20019119 (maps 9011, 9012, 9021, 9031), the axe
20019120 (Hands to Spare, 10000098) and the Traces of the Past objects 20019047-20019049
(10000061). Objects gated by `ConsumeItem`, `PlayRecord` or channel-event rows stay unexported.

At runtime `adventure::Session` checks the conditions before an interaction start or an
instant use of an object that is not yet activated, through `QuestsSession::requirements_met`
(the same `WorldQuestBook` evaluator quest acceptance uses). A failed check, or a session
without a quest book, answers `Interaction_ConditionsNotMatch`. An activated object replays its
committed result without checking or granting again. When the activation commits, the world
session runs the quest state refresh immediately, so `AdventureObjectStatus` and `GetItem`
objectives update right after the reward instead of at the next time sync.

### Company conditions and level unlocks

`Company` quest conditions are evaluated against `CompanyService` in the client's own order
(`AAC_Requirement_CheckCompany_Candidate`, `dumps/ida_research/company-requirement-20261030`):
the same company; a valid pioneer permit unless `ignore_permit`; position level of at least
`level`; with `is_max_contribution`, contribution held exactly at the ceiling of a level flagged
`isMaxPositionLevel` (Blue Salt level 2: 1500, level 3: 11500); and at least `contribution`.
Without a company service the condition fails closed. 290 catalog quests carry such a condition;
six became acceptable, including Donella's housing quests 80101045 (needs a Blue Salt pioneer
permit and 500 contribution) and 20001072.

`CompanyLevelUnlock` rewards (quest 80101045 unlocks Blue Salt level 2, quest 20001072 level 3)
are stored in the catalog's `company_unlocks`. Company standing lives outside world progress,
so the unlock is applied through `CompanyService::unlock_position` after the quest completion
commits; a failed commit applies nothing. Unlocks only raise a level, and world entry re-applies
the unlocks of every completed quest, which repairs an unlock lost between the two writes.
Before this change both quests were refused with `Quest_IsNotAcceptable` ("cannot start this
quest") because the catalog marked `condition:Company` and `reward:CompanyLevelUnlock`
unsupported.

`CompanyContribution` rewards (152 quests) are stored as `company_contribution` and credited to
the character's current company through `CompanyService::add_contribution` after the completion
commits, unlocks first, followed by `PS2C_CompanyUpdateNoti`. The rows name no company. Credit
is at most once: a failed commit credits nothing, and a failure after the commit loses the
points rather than repeating them, so world entry does not replay contribution. Donella's hidden
quest 90116021, accepted while turning in 80101045, grants the single point that moves a capped
Blue Salt level 2 member (1500) to level 3 (1501), which Nivhen's rent selection requires.

## Live hotfix (2026-11-01)

- **Equipment from world objects.** First Hunt's hunting knife and chests that roll equipment
  failed: the adventure reward named the grade `EX5ItemGrade_Normal`, and the tutorial reward
  commit, which adds the prefix itself, rejected it. The request failure closed the connection.
  The reward now carries the catalog grade name, and an adventure failure refuses only that
  request (`adventure::Session::abort`) instead of dropping the player.
- **Full wallet.** The graduation test kit fills the wallet to the Money limit
  (9,000,000,000,000,000). Every gold-paying turn-in then answered BadRequest and never completed
  (live: quest 10000046). CUSTOM: quest, supply and adventure money rewards, loot kill gold, NPC
  sale proceeds, trade deliveries, achievement rewards and draw boxes now fill the wallet to the
  limit and grant the rest. The official rule for a full wallet is not captured.
- **Profiles ship together.** The live host had the full quest catalog but older gathering and
  NPC placement profiles, so quest doodads and some quest NPCs were missing. Deploy
  `quests/catalog.json`, `gathering/definitions.json` and `npc/placements/` from the same tools.

## Equipment rewards and instance quests (2026-11-01)

- **Equipment rewards are items.** Quest rules credit every reward, supply and rolled loot pack
  as a stack. For items stored as instances (equipment, tools) `QuestsSession::commit` moves the
  credited count into new item rows in the same transaction and the `ItemsUpdateNoti` lists
  those rows. A full bag refuses the turn-in with `ItemSlotLimit` and commits nothing.
- **Graduation.** The graduation quest pass drops misfiled equipment stacks only when this pass
  created them; deleting a stored row rejected the whole pass.
- **Instances.** Quest content inside caves, checkpoints and dungeons is reachable through the
  map travel planner ([instances-and-dungeons.md](instances-and-dungeons.md)); a reconnect
  inside an instance resumes there with its quest objectives.

## Missing Officer and the Cliffs of Madness (2026-11-01)

The verified client data and P2 official capture establish this route: Jennifer's **Missing
Husband** (20001051) completes through Renald's dialogue. Renald offers **Missing Officer**
(20001052), and the Pirate Hideout gate requires the previous quest. The first objective is
entering the Cliffs of Madness; the second is finding Edward. The official server accepts
**Rescue Plan** (20001053) immediately after that quest completes, and **Alight the Cliffs**
(20001054) immediately after the rescue. The latter leads to the sequential pirate captain
quest 10000146, then to **To the Pioneers' Assembly Hall** (10000133). See
[instances-and-dungeons.md](instances-and-dungeons.md) for the entrance.

The local server now starts the four captured follow-ups after their predecessors and repairs
missing follow-ups on periodic quest refresh for existing characters, without restarting a quest
that was deliberately abandoned. Three later authored QuestComplete transitions are also
started and repaired: the assembly-hall follow-up, Settling a Debt after quest 10000149, and
An Offer You Can't Refuse after Settling a Debt. The timing of these three starts is a local
candidate; unlike the first four, it has not been confirmed in an official capture. Settling a
Debt still requires completion of quest 10000056. The quest exporter accepts
both observed sequential-group formats; the regenerated catalog adds 100 missing successor links,
all targeting sequential quests. These links alone do not establish that every linked quest is
playable.

The prison key is an individual item in the official capture. Its adventure-object reward now
uses that item form, and the prison door consumes it in the same revision-checked commit as the
interaction. A legacy stacked key is also consumed. The cannon's instant interaction uses the
actual InteractUnitReq shape, plays its captured scene and opens the linked barrier. The barrier
state is shared for the life of the server process; a restart restores its authored state, and
the cannon can be used again. The instance itself is still shared by map rather than allocated
per party. Original-client acceptance of the complete route remains pending.

## Verification and boundaries

```powershell
python tools/build_tutorial_data.py
python tools/test_tutorial_quests.py
$env:CHRONICLES_TUTORIAL_DEFINITIONS = (Resolve-Path data/generated/tutorial/definitions.json).Path
cmake --build --preset release --target chronicles-tests
ctest --preset release -R 'Quest|Tutorial'
```

`GeneratedQuests.InitialTutorialCompletesEveryQuestAndRestoresItsRewardsAndObjects`
drives the generated map 8000 definitions through all seven quests, injecting
trusted death events at the combat boundary. It checks the alternative trigger
group, dialogue, both quest objects, successor acceptance, rewards, and saved
snapshots. This is a synthetic quest integration test, not an original-client
playthrough or a test of combat damage calculation.

`WorldQuests.ServerMonsterDeathAdvancesHuntsOnceAndKeepsSequentialGates` checks a
synthetic shared-world hunt event and duplicate kill receipt. `native_world.cpp` routes
server-owned monster defeats to the shared quest book; original-client hunt progression
has not been verified.

The followup test covers NPC acceptance, exactly-once supply items, initial
progress, and the hidden quest in map 10117. Quest 10000046 continues into the
shared world, where the all-quest catalog takes over its remaining objectives.

On 2026-10-26 the synthetic `WorldQuests`, `QuestCatalog`, `LevelTriggers` and
`GeneratedQuests` tests, the real-socket
`NativeWorld.TutorialCutsceneFollowUpsTeleportAndMoveMapsOverTheRealFramePath`, and the
Python extraction tests passed. The full `chronicles-tests` run passed 518 tests with 33
skips; three NativeWorld tests that also crash on the unmodified main build were excluded.
No shared-world quest has been played in the original client against this server.

```powershell
python tools/build_quest_catalog.py
python -m unittest discover -s tools -p "test_build_quest_catalog.py"
$env:CHRONICLES_QUEST_CATALOG = (Resolve-Path data/generated/quests/catalog.json).Path
```

The isolated PostgreSQL runner `tools/test_postgres.py` exercises durable quest
counters and kill receipts, ownership, stale revisions, and rejected resets in
`TutorialStorage.PostgresOwnershipAtomicityAndRevisionConflict`. It does not
modify the running development database.

On 2026-10-24, all ten focused quest/definition tests passed with the complete
generated tutorial profile, including the two generated-profile integration
tests. The PostgreSQL receipt/counter test also passed. Its isolated runner
passed account and character restart persistence and shut the test cluster down
successfully. These results do not establish an original-client quest playthrough.

On 2026-10-30 the objective audit added `WorldQuests.CommittedCraftsAndDoodadInteractionsAdvanceCounterObjectives`,
`MemoReportsNeedThePlacedBoardNearThePlayer`, `InteractNpcReportsConfirmTheDialogueCredit` and
`TurnInConsumesTheCollectedStacks`, plus the exporter case for memo, crafting and Ethereal
objectives. The default Release suite ran 2,088 cases with 0 failures, and the real catalog loads
with its memo placements. Words of Warning, Pioneer's Remedy and Gathering Bone Fragments have
not yet been replayed in the original client against this server. A character that crafted the
potion before this change must craft it again: the earlier craft was never recorded.
