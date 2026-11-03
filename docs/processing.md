# Crafting stations: processing and campfire cooking

This page describes the emulator's crafting-station service: the in-game
**Processing** categories (Food, Wildcraft, Cloth, Refining, Smelting), campfire
cooking, and the other station recipes the client export marks as known by
default. Definitions and timings apply to client build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa` (executable dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`).

**Status: partial.** The service is wired into tutorial and shared-world sessions. The current
profile includes demo ArcheWorld maps and the shipped housing area (`20101`). A local client
session has reached that housing scene, but construction/cooking acceptance of the updated
station service remains unverified. Synthetic tests establish the bounded server behavior.

## Evidence

Statements below come from the hash-checked client export and the sanitized
official captures described in `docs/capture-corpus.md`. No player, character,
item-instance or session values are copied into data or documents.

| Finding | Evidence |
| --- | --- |
| A station session starts with `PC2S_DialogueStartReq` on the station; the reply is `PS2C_DialogueStartRes` with `attach_pos`/`attach_yaw` equal to the request pose. `PC2S_DialogueEndReq` is answered by `PS2C_DialogueEndNoti`. | Eight own dialogues on placed stations in Generic Parts 1 and 2. |
| A craft is `PC2S_StartInteractionAnimReq` with `param.interaction.execute_craft` (`item_craft_id`, `count`) and `interaction_id`; the server answers `PS2C_StartInteractionAnimNoti`. | Nine own crafts (2 in Part 1, 7 in Part 2), all `count` 1. |
| `target_unit_id` is the spot unit ID `(sector_id << 32) \| (local_id << 12) \| 6`. | All captured targets resolved to placements of doodad 20251 (campfire). |
| A station's interaction comes from `Doodad.interactionGroupId` or its `DoodadPhase` row; its recipes are the `ItemCraftPack` IDs in `Doodad.propertyType`. | Doodad 20251 phase 1 → interaction 10220 (Craft, animation 60001); pack 3000001 contains recipe 60190101. |
| `ItemCraft` gives materials, Gold cost and product. | Every capture: 9 × 70102001 and 125 Gold became 5 × 40101101. |
| Execute is accepted at the montage's `interaction_execute` notify. | Execute followed the start by 2,937 ms; the 60001 montage notify is at 2,933 ms. |
| Success order: `ItemsUpdateNoti` (`craft_action` reason, `money_changes`, product then material stack totals), `OnDemandDynamicCharItemCraftResultNoti` (recipe and produced items), `InteractionAnimRes`; the end request receives another `InteractionAnimRes`. | All nine captured crafts. The official server also sent two `CharPlayRecordUpdateNoti` and, during a quest, `QuestUpdateNoti`. |

The Processing categories map to `ItemCraft.expertiseType` and their village
stations: Food → Milling (doodad 81002), Wildcraft → Carpentry (81003),
Cloth → Trimming (81004), Refining → Alchemy (81009), Smelting → Smelting
(81001). The category-to-expertise mapping is inferred from the UI labels and
recipe groups. The original three captures only recorded campfire cooking.

### Additional official capture reviewed 2026-10-26

The fourth capture adds two count-1 Smelting results for recipe 60203201
(product 70203002, Metal craft material) and three count-1 Carpentry results for
recipe 60207101 (product 70207001, Lumber craft material). Own craft starts,
result notifications and material/Gold changes agree with the static recipes.
These flows use animations 60006 and 60004 and interactions 602 and 604.
They extend the evidence beyond the original nine Cooking results; they do not
prove local original-client acceptance of ProcessingSession or expert-level
requirements. Additional gathering requests use animation 1010 twice and
animation 1031 seven times. Animation 11150 only establishes a generic Use task;
its purpose remains unresolved.

## Building the profile

```powershell
cd 'D:\PROJETOS\Archeage Emulator\Emulator'
python tools/build_processing_data.py
python -m unittest discover -s tools -p 'test_build_processing_data.py'
```

The builder reads `data/generated` (see `docs/client-data.md`), verifies catalog
and raw hashes through the export manifests, and writes the ignored
`data/generated/processing/definitions.json` with its source hashes. The current
export yields 111 placements, 21 station definitions and 233 recipes across 28 eligible map
areas. Processing explicitly opts into the
Release HousingArea bounds; the shared gathering builder's default remains demo-only. The server loads the
profile at startup when `--client-data-dir` and the world bootstrap diagnostic
are enabled and the profile exists next to the catalog.

Stations without a map placement are exported when a housing lot can hold them
(`HousingObject`/`HousingBuilding` `Item.FK_doodadId`, `HousingPresetDoodad`,
`HousingModelPartAddOn`): the Smelting Furnace, Kitchen, Atelier, Refinery and Spagyric Table
(doodads 85001-85005). Players place these at runtime; the world session resolves their
position from the lot (see "Stations placed on a housing lot" below). A lot-only station whose
recipes contradict its interaction's expertise is omitted and listed under
`unsupported_stations` (building add-ons 169990 and 170000); the same contradiction at a
map station still fails the build.

The builder fails closed on unknown items or packs, repeated materials, invalid
counts, craft animations with a cost or tool, a station offering more than one
craft interaction group, and a placed station offering recipes of another
expertise. Conditional craft interactions (700–702) and the animation-less
trade-good interaction (601) are not offered. Recipes are reported unsupported
when they must be learned (`autoRegistration` False, 124 rows), consume equipment
instances (3), or otherwise require unsupported costs/company-position handling. The 17
building recipes now persist their products in housing storage and consume blueprint stacks.

## Runtime flow

`src/world/features/processing/processing.*` owns the rules; `src/world/session/native_world.cpp` routes requests and
supplies storage.

1. **Open.** `DialogueStartReq` on a station placed inside the current map's
   area is answered with `DialogueStartRes` and remembers the open station. The
   request position must be within 5 m of the server position and the station
   within 7 m (the tutorial interaction allowance; captured crafts started
   1.25–1.27 m from the station origin).
2. **Start.** The craft must target the open station, use an interaction from
   the station's group and that interaction's animation, name a supported
   recipe from the station's packs, and request a count from 1 to
   `MaxNumOfCraftAmount` (999). Materials, Gold and inventory space are checked
   now so the client gets an immediate refusal.
3. **Execute.** Accepted from the server-side execute time minus a 100 ms
   polling allowance until 30 s after the start, with the player still in
   range. The craft is planned again from fresh storage; materials, Gold and
   products are committed in one revision-checked
   `commit_world_item_trade` transaction. A retried execute returns
   `InteractionAnimRes` without crafting again.
4. **End.** Ending the animation, or the station dialogue, before execution
   cancels the craft without consuming anything.

`count` multiplies materials, Gold and products linearly in one execute. This
is an inference: every captured craft used count 1.

### Stations placed on a housing lot

`ProcessingSession` takes an optional `ProcessingStationLocator`. A dialogue or craft target
that is not an exported map placement is passed to it, and the result is served only when its
doodad is a profiled station. Each placement carries its own reach; map stations keep 7 m.
The world session's locator asks `HousingHub::placed_doodad` for an object on a lot of the
character's current housing channel and map, and returns the lot centre with a reach of the
lot's corner distance plus 7 m (emulator policy: lot-local positions compose with transforms
the server does not reproduce). The housing furniture-pose handler, which runs earlier in the
frame, leaves requests with `param.interaction.execute_craft` to processing.

Live log 2026-11-02: before this change a placed Kitchen answered `DialogueStartReq` with
`Interaction_TargetNotFound` ("Unable to find target").

### Player unit identity

`DialogueEndNoti.player_unit_id` and `InteractionAnimRes.unit_id` name the player's
wire unit ID, `(character_id << 4) | 1` (`player_wire_unit_id`), which is the ID the
client receives in `StartPlayingRes`. Every official own station end names the own
player unit. The native adapter originally gave the session the raw character ID.
In a local original-client trial on 2026-10-27 the end notification named the
character ID, and the character stayed attached to the station and could not move
until relog. The gathering session had the same defect in its own
`StartInteractionAnimNoti` and result packets; both now receive the wire unit ID.
Storage still uses the raw character ID.

### Crafting entry snapshots

Every official world entry sends `WorldCharExpertiseSkillsNoti` (opcode 128) before
`WorldCharItemsNoti` and an empty, non-update `WorldCharItemCraftsNoti` (132) after it.
The expertise snapshot lists all sixteen `EX5ExpertiseType` values in enum order at
`EX5ExpertiseLvType_Lv1`, including `TradeItem`, which has no `ExpertiseSkill` row. The
emulator now sends both in that order on every entry (`expertise_skills_entry_snapshot`).

They are required to use any station. In the matching client build,
`AAC_Interaction_Craft_OpenUiRequiresExpertise` (0x1461290B0) opens the craft window
for a Craft interaction only when `FX5CraftManager` reports a non-zero level for the
interaction's `expertiseType`. The level store is filled only from this snapshot and
skips Lv0 entries. Without it the client raises `Craft_SkillNotLearn` (1104, tag
`InteractionUseOpenUi`) and ends the dialogue. That is what a local trial on 2026-10-27
showed at the map-9012 Brewing Furnace (doodad 20251, interaction 10220, expertise
`Jewealry`): `DialogueEndReq` arrived 17 ms after `DialogueStartRes`, before any craft.
The function names, comments and pseudocode are recorded under
`dumps/ida_research/processing-expertise-20261027/`.

Stackable products (`Consumable`, `CraftMaterial` and the other categories the
NPC shop stores as stacks) increase `WorldProgress::stackables`. `Equipment`
products become new item instances with the catalog's maximum durability and
grade, at most 32 per craft. A depleted material keeps its stack row at zero,
because storage never deletes progress rows. Utility-bag reservations are not
consumed. `HousingBuilding`/`HousingObject` products increase housing-item quantity while
preserving `installed_count`; `HousingBuildingDrawing` materials are ordinary stacks. The
housing quantity and material/gold changes share one revisioned commit and are reported through
`ItemsUpdateNoti.housing_items` and `stackables`.

Sessions with a private tutorial instance commit from the tutorial's cached
progress revision and then adopt the result, as the NPC shop and corpse loot
do; other sessions read storage directly.

| Rejection | Error code | Packet |
| --- | --- | --- |
| Dialogue target not a placed station on this map | not claimed; other services decide | — |
| Housing station not on a lot of the character's channel, or not a profiled station | not claimed; other services decide | — |
| Dialogue request position disagrees with the server | `Dialogue_InvalidPos` | `DialogueStartRes` |
| Station farther than 7 m | `TooFar` | `DialogueStartRes` / `StartInteractionAnimRes` / `CommonError` |
| Craft target is not a placed station on this map | `Craft_NotFoundCraftingTable` | `StartInteractionAnimRes` |
| Craft target is not the open station | `Craft_InvalidCraftingTable` | `StartInteractionAnimRes` |
| Interaction outside the station's group | `Interaction_GroupIdNotMatch` | `StartInteractionAnimRes` |
| Animation differs from the interaction's | `InteractionAnim_Mismatch` | `StartInteractionAnimRes` |
| Recipe not in the station's packs | `Craft_CraftPackNotMatch` | `StartInteractionAnimRes` |
| Learned/equipment-material or other unsupported recipe | `Craft_Restricted` | `StartInteractionAnimRes` |
| Count outside 1–999 | `Craft_InvalidReqCountNumber` | `StartInteractionAnimRes` |
| Extra material instances supplied | `Craft_WrongAddMaterial` | `StartInteractionAnimRes` |
| Not enough materials (after utility-bag reservations) | `Craft_MaterialNotEnough` | start or execute |
| Not enough Gold | `MoneyNotEnough` | start or execute |
| No inventory slot or stack kind left | `ItemSlotLimit` | start or execute |
| Another action is open | `InteractionAnim_UsingSkill` | `StartInteractionAnimRes` |
| Execute before the deadline or after expiry | `InteractionAnim_TimeNotMatch` | `CommonError` |
| Commit conflict or storage failure | `Craft_FailCraftAction` | `CommonError` |
| Malformed request or more than 32 requests per second | `BadRequest` | by request |

No official craft rejection was captured. Start rejections follow the captured
`StartInteractionAnimRes` rejection shape; execute rejections use
`PS2C_CommonError`, the gathering and tutorial-interaction precedent.

## Limits and remaining work

- Updated construction-workshop UI/crafting acceptance still needs a live original-client trial.
- After the expertise-snapshot fix (see "Crafting entry snapshots"), an original-client trial
  must still confirm that the Brewing Furnace window opens and that a craft completes end to
  end. Shared-world `SectorUpdateNoti` streaming is now sent as on the official server
  (`docs/sector-streaming.md`); static analysis showed its absence was not what closed the
  station window.
- Expertise experience, expertise level requirements (`properExpertiseLevel`,
  `tierLimit`) and level benefits are not applied. Every character reports Lv1 in all
  sixteen expertise types, the official new-character snapshot.
- Learned recipes (`WorldCharItemCraftsNoti.craft_ids`),
  instance materials, company position requirements and craft success or bonus
  chances are not implemented.
- Play-record counters and quest progress that the official server sent with
  craft results are not sent.
- The official server sent `MoveRollback` (`BadRequest`) right after station
  dialogues and craft starts; the emulator does not, and its cause is unknown.
- Other players do not receive the crafter's `StartInteractionAnimNoti` or
  `DialogueStartNoti`.
- A second unit ID per station (spot kind 7) received a `DialogueStartRes`
  without an attach pose in one capture; its purpose is unknown and it is not
  handled.
- Weight is not enforced.

## Verification

```powershell
python tools/build_processing_data.py
python -m unittest discover -s tools -p 'test_build_processing_data.py'
cmake --build --preset release --target chronicles-tests
ctest --preset release -R 'Processing'
```

The C++ tests use the in-memory store and cover the captured campfire flow at
the server deadline, early and retried executes, the dialogue requirement,
station pack/interaction/animation checks, unsupported recipes, count bounds,
missing materials or Gold at start and at execute, utility-bag reservations,
equipment instance grants, map and range checks, commit failure, overlapping
actions, cancellation, request bursts, profile validation and wire encoding of
every reply. Set `CHRONICLES_PROCESSING_DEFINITIONS` to the generated profile to
also check the captured campfire and housing Construction Workshop against real data. The
2026-10-26 housing follow-up passed the 58-test focused native-housing/static-NPC/processing/
merchant selection, including real-data construction mapping, housing-product consumption,
installed-count preservation and overflow refusal. The processing Python suite passed 10 tests
and the gathering suite passed 9, preserving gathering's default map scope. An original-client
trial is still required before claiming workshop compatibility.

The 2026-10-27 unit-identity fix adds the TLS regression
`NativeWorld.CraftingStationDialogueEndReleasesThePlayersOwnWireUnit`, which opens and
ends a synthetic station dialogue through the real world session. It failed against the
old construction (unit 1 instead of wire unit 17) and passes with the fix, together with
36 focused Processing and Gathering cases:

```powershell
cmake --build --preset release --target chronicles-tests
ctest --preset release -R 'NativeWorld.CraftingStationDialogueEnd.*|Processing.*|.*Gathering.*'
```

The corrected server has not yet been retried with the original client.

The 2026-11-02 housing station repair adds
`NativeWorld.CraftingStationPlacedOnAHousingLotOpensAndCraftsOverTls` (rent, enter map 20101,
save a Kitchen, open it and complete a craft over the real world socket; it fails with the
locator disabled), `HousingWorldTest.PlacedObjectsResolveToTheirLotForOtherServices` and the
processing cases `StationsFromTheLocatorOpenAndCraftLikeMapStations` and
`LocatorStationsOutsideTheirReachOrMapAreRejected`. The processing Python suite passes 12
tests. Crafting at a placed station in the original client is still unverified.

The 2026-10-27 expertise-snapshot fix is covered by
`NativeWorld.OptInTutorialMoveFollowsClientMapAndPlayingRequests`, whose handshake check
decodes `WorldCharExpertiseSkillsNoti` (sixteen types, all Lv1, `Jewealry` and
`TradeItem` present) before the inventory and the empty `WorldCharItemCraftsNoti` after it.
Every TLS entry-sequence test reads the pair through `read_entry_inventory`. A Release run of
`NativeWorld.*:CharacterStats.*:Processing*:*Gathering*` passed 134 cases, with 34 data-gated
skips and the known `BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal` access
violation. With the generated catalog and tutorial profile, two tutorial TLS tests stop with
the same kind of test-thread access violation after successful server exchanges; every
available test binary already contains this change, so their earlier status is not
established. The rebuilt local server reports healthy. Opening the Brewing Furnace and
completing a craft in the original client is still the acceptance check.
