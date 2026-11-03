# Housing

The emulator serves personal housing, guild housing areas, visits, building/decorating, door
and window toggles, move packages, housing storage, contract renewal/expiry and the GM housing
commands. Rules come from the verified client catalog; state is persisted in memory or
PostgreSQL (schema version 18) together with the character payments that buy it.

The server-side implementation has synthetic coverage, including a real TLS world socket test.
Local original-client sessions have confirmed GM lot rental, housing map entry, scene
presentation and one ordinary Residence Entry List visit and return. Opening Nivhen's housing
window failed with `Housing_ChannelNotFound` until the 2026-10-29 board repair; ordinary rental,
construction and editing still need original-client acceptance on the updated server.

## Owning sources

| Layer | Sources |
| --- | --- |
| Catalog (static rules) | `src/world/features/housing/housing_catalog.*`, `src/world/features/housing/housing_doodads.*` (garden profile from `tools/build_housing_data.py`) |
| Domain service (transport-free) | `src/world/features/housing/housing_service.*`, `src/world/features/housing/housing_guild_service.cpp`, `src/world/features/housing/housing_state.hpp` |
| Persistence | `src/world/features/housing/housing_storage.cpp`, `src/persistence/postgres/character_rows.hpp`, `migrations/001_accounts_characters.sql` |
| World transport (PC2S/PS2C) | `src/world/features/housing/housing_world.*` (`HousingHub`), `src/world/session/native_world.cpp` (session travel, entry, presence) |
| Front WebSocket (WSC2F/WSF2C) | `src/world/features/housing/native_housing.*`, `src/services/front/native_ws.cpp` |
| Startup wiring | `src/apps/server.cpp` (`create_housing_hub`, guild event subscription) |
| Housing permit stacks | `src/data/shop_catalog.cpp` (`HousingPermit` is a stackable category) |

Tests: `chronicles-housing-tests` (`housing_catalog_tests.cpp`, `housing_service_tests.cpp`,
`housing_storage_tests.cpp`, `housing_world_tests.cpp`, isolated PostgreSQL tests),
`NativeWorld.HousingContractVisitEditAndExitOverTls` in `chronicles-world-session-tests`, and
the housing shop tests in `npc_shop_service_tests.cpp` (`chronicles-world-features-shop-tests`).
`HousingWorldTest.StockClientSaveOrderPlacesFurnitureThatReentryShows` replays the client's
end-then-save order; the furniture pose tests are `PlacedChairsOfferTheirPoseToEveryoneInTheChannel`,
`FurniturePosesRefuseUnknownObjectsAndAnimations` and
`SeatedPlayersArePresentedToLaterViewersUntilTheyStandOrLeave`. Gardens are covered by
`housing_doodads_tests.cpp`, the `Garden*` service and world tests and
`tools/test_build_housing_data.py`.

Generate the garden profile with `python tools/build_housing_data.py`; without
`data/generated/housing/doodads.json` the server logs a warning and refuses garden interactions.

## Game data (client build `40367e24...`)

- 71 `HousingSite` lots: `Zone_01_Housing_001` has 41 (37 small, 3 medium, 1 large) and
  `Zone_07_Housing_001` has 30. Only `Zone_01_Housing_001` has a `HousingArea` map (20101) in
  this build, so it is the only rentable zone; the other zone is reported but refused.
- `HousingRentCost`: every contract costs item 123 (housing permit). Small 60 permits, medium
  240 permits plus a one-time 1,000 Hersander Token unlock, large 480 permits plus 2,000 Tokens,
  guild area 3,000 permits plus 5,000 Tokens.
- `GameVariable`: 30-day site and area terms with a 7-day imminent window, 5 s edit-save
  cooldown, 60 min guild-site edit cooldown, 180 s GoHome cooldown, object limits 160/180/200
  and farm-doodad limits 5/10/15 for small/medium/large lots.
- `HousingPreset`, `HousingPresetDoodad` and `HousingModelPartAddOn` provide each lot's starting
  buildings and objects; `HousingUpgrade` provides building upgrade materials.

## How it works

### Renting a lot (front `WSC2F_HousingSiteContractReq` 20011 -> `WSF2C_HousingSiteContractRes` 17)

The request names only site size, zone and contract type; the server picks the first free lot
of that size, opening another channel (copy of the zone) when every channel is full, up to the
channel limit. The charge (permits, and the Token unlock the first time a size is rented) and
the new site commit in one transaction. The owner receives `PS2C_CharHousingSiteUpdateNoti` (84)
and `PS2C_ItemsUpdateNoti` (169) with the spent permits; characters in that channel receive
`PS2C_HousingAddNoti` (200). One personal site per character.

A player's contract also needs the company position level of the site size, from GameVariable
`GeneralHousingSite{S,M,L}positionLv` (3, 4 and 5 in this build); a lower position is refused
with `Housing_InsufficientCompanyPosition` before anything is charged. The front adapter reads
the position from `CompanyService`. Candidate: the server checks the character's current company
position; the client's own dialogue additionally requires Blue Salt membership (below). The
emulator-defined GM `housing_contract` command is not gated by company standing.

`WSC2F_HousingUnlockGetReq` (20018) reports unlocked and rentable sizes and channel limits;
`WSC2F_HousingSiteAreaBoardGetReq` (20012) lists a channel's residents.

### Opening the housing window from the NPC (Nivhen)

Nivhen (Npc 1001018) opens dialogue 115000 when the character has completed quest 80101045 and
owns no house. Its rent selections run the `HousingSiteAuthorize` PlayerTask for
`ZONE_01_HOUSING_001`:

- 115000/1/2 leads to 115002, whose 115002/1/1 selection needs no existing house, Blue Salt
  membership at company level 3 or higher and quest 160000 acceptable (condition 500012223).
- 115000/1/3 serves a guild master who completed quest 160000 and whose guild has no area.

Static client analysis (`dumps/ida_research/housing-ordinary-20261029`) shows that
`HousingSiteAuthorize` (PlayerTask case 51) sends no packet and waits for no reply. The client
checks locally that the zone has HousingSite rows and that the character owns no site, raises a
local housing event and opens the rent window after the conversation. That window then sends the
front requests the server already serves: the move-package list (20017), the resident board
(20012) and, on confirmation, `HousingSiteContractReq` (20011) with the dialogue's zone, the
selected size and contract type 1. No server dialogue handler is needed and none is added; the
dialogue conditions are the client's progression gate, and the server enforces the site rules
above.

The board context of a window opened in the public world has channel 0. The stock client
labels channel 0 as the main-world board (`TXT_HOUSING_HOUSINGAREABOARD_MAINWORLD`), and the
board reply carries no channel. The server resolves channel 0 to the character's live channel in
that zone, otherwise to the primary personal channel 1, which exists before anyone rents. This
is an emulator policy; it is not proof that the official server maps channel 0 to channel 1.
Before this change the server refused channel 0 with 2041 `Housing_ChannelNotFound`, which the
client reported when the window opened.

### Visiting (`WSC2F_HousingChannelList/Search/VisitReq` 20013-20015, `PC2S_GoHomeReq` 20083)

Channels can be listed or searched by owner, guild or area name. The Residence Entry List visits
the channel of the selected list row: the client sends `20015 {channel}` with that row's
channel and never synthesizes one, so a zero channel is still refused. A local original-client
session on 2026-10-29 sent channel list 20013 and then visit 20015 with an explicit channel; the
server moved the character into map 20101, received its map-load completion and later returned
it to its origin. A visit (or GoHome to the
character's own lot, with the 180 s cooldown) moves the character into map 20101 on its own
world connection: `PS2C_MapMoveStartNoti` (175), then after the client's `MapLoadCompleteReq`,
`PS2C_MapMoveCompleteNoti` (176) followed by `HousingAddNoti` for every occupied lot of that
channel. Visitors of one channel see each other; other channels of the same map do not.
`PC2S_ZoneChannelMoveReq` (20090) while inside the area returns the character to where it came
from. When no origin is known (after a restart, for a character that logged in inside the area,
or one that walked in), it steps out onto the area's parent map at the point the request was sent
from (bounded to five metres of the tracked position): map 20101 shares map 9012's coordinates
and lies inside its area. The tutorial start is used only when the travel profile names no parent
arrival. Before 2026-11-02 the unknown-origin case always went to the tutorial start, which a
live player reported as being teleported back to the tutorial. Walking into the area's cells from
map 9012 enters the character's own channel there (else channel 1) where it stands, without a
recorded origin (see [zone-channel-travel.md](zone-channel-travel.md)).
A character saved inside the housing map re-enters its channel at the saved spot on the next
login. GM `tp` and `up/down/fwd` typed inside the area build their destination on map 9012,
so they leave the area; before 2026-11-02 they stayed on map 20101 and showed an empty map.

### Building and decorating (`PC2S_HousingEditModeReq` 20040, `PC2S_UpdateHousingReq` 20045)

Edit mode needs the owner standing on its own lot in the current channel; visitors see
`PS2C_HousingEditModeNoti` (83). `UpdateHousingReq` places, moves and removes buildings and
objects. Each placement is checked against the catalog (known housing/doodad, inside the lot's
design bounds, finite rotation, object and farm limits, a main building before attached ones,
upgrade materials) and against housing storage: placing consumes one stored item, removing an
owned object returns it to storage, preset objects return nothing. The edit-save cooldown
applies. The owner and visitors receive `PS2C_UpdateHousingRes` (202); storage changes arrive in
`ItemsUpdateNoti.housing_items`.

The stock client saves after it leaves edit mode. Pressing save sends
`HousingEditModeReq{mode, end: true}`; the client waits for `HousingEditModeNoti{end: true}` and
only then sends `UpdateHousingReq` with its changes (`AAC_PS2C_HousingEditModeNoti_Handle` ->
`AAC_Housing_ExitBuildMode_SaveOrDiscard`, static analysis in
`dumps/ida_research/housing-furniture-20261030`). The live server on 2026-10-30 logged exactly
that order about 160 ms apart. Until then the server discarded the edit session on `end` and
refused every save with `Housing_InvalidDoodadUpdateReq`, so no furniture was ever stored. An
ended session now accepts one trailing save to the same lot within 30 seconds (emulator
policy); that save closes the session whether it succeeds or not. Saves sent while still in
edit mode keep working. Every edit-mode request and save is logged with the character ID, the
object counts and the result code, and a refused save also logs the object and catalog IDs with
their lot-local positions.

`HouseNub.attach_index` is optional, and the client sets it only for attached buildings
(`AAC_Housing_BuildEditor_FillHouseNub`, static analysis in
`dumps/ida_research/housing-style-20261031`). The server therefore reads an absent value as -1,
a main building. Before 2026-10-31 it read 0, so every main-building move was refused with
`Housing_InvalidAttachIndex`. A move keeps the building's door/window state, preset ownership
and the objects placed inside it, which stay attached to the building.

Crafting stations (Kitchen, Spagyric Table, Atelier, Refinery and Smelting Furnace) are
`HousingBuilding` items that name both a `Housing` row and a doodad (`Item.FK_doodadId`
85001-85005). The stock client places them from housing storage as a doodad of that ID,
attached to the lot's main building; the live server on 2026-10-31 logged
`doodads=[ 1/11:85002@7349,9695,0^0 ]`. The catalog used to map doodads only for
`HousingObject` items, so every station save was refused with `Housing_InvalidCmsId` and
nothing was stored. Building items now also resolve their doodad: the save installs one stored
copy, and removal, move packages and contract release return it like furniture.

A placed station is used like a village station: `DialogueStartReq` (20065) on its housing
doodad unit ID, then the craft `StartInteractionAnimReq` (20069 with
`param.interaction.execute_craft`), `ExecuteInteractionReq` and `EndInteractionAnimReq`. Until
2026-11-02 processing knew only exported map stations, so the live server answered the
dialogue with `Interaction_TargetNotFound` ("Unable to find target"), and the furniture-pose
handler would have refused the craft with `Interaction_Invalid`. Now
`HousingHub::placed_doodad` resolves the unit ID on a lot of the character's current channel
and HousingArea map, the world session hands it to `ProcessingSession` as a runtime station
placement, and the pose handler leaves crafts alone. The processing profile exports the five
lot stations with their own interaction groups and recipe packs (see `docs/processing.md`).

Emulator policy: object positions are lot-local and compose with the lot's and a parent
building's transforms, which the server does not reproduce. A placed station therefore
accepts a character within the lot's corner distance plus the ordinary 7 m station allowance
of the lot centre. Candidate: anyone in the lot's channel may use a placed station, as with
furniture; each crafter spends their own materials.

### Furniture poses (`PC2S_StartInteractionAnimReq` 20069, `PC2S_EndInteractionAnimReq` 20071)

Chairs and beds offer catalog poses: the doodad's `interactionGroupId` names unconditional `Use`
interactions whose `InteractionAnim` costs nothing and sends no execute request
(`noExecuteNotify`), for example interaction 70 / animation 70 (`Chair_Sit_01_INT`, hold after
end) and interaction 73 / animation 71 (release). An interaction start aimed at a placed housing
doodad (`target_unit_id` is the doodad unit ID) is served when that doodad stands on an occupied
lot of the requester's channel and offers the animation. Anyone in the channel may use
furniture, as with doors.

Success answers `PS2C_StartInteractionAnimNoti` (166) to the requester and everyone in the
channel, echoing the request's param, position, yaw and tick and naming the furniture as
`target_unit_id`. The matching end answers `PS2C_InteractionAnimRes` (167) with the unit and the
ended animation to the same audience. Refusals answer `PS2C_StartInteractionAnimRes` (165) with
`Housing_NotRegisteredHousingSite`, `Housing_DoodadNotFound` or `Interaction_Invalid`. Requests
for any other target continue to the statue, processing and gathering handlers. Candidate: no
official furniture capture exists; the packets mirror the captured gathering flow.

A pose whose animation holds after its end (`attachPolicy` `HoldAfterEnd`, sitting) is kept for
viewers who see the player later: when the presence system announces the seated player's unit
to a newcomer, the stored `StartInteractionAnimNoti` follows it, as the selfie pose already
does. Standing up (`ReleaseHeld`), starting another interaction, changing channel or moving more
than 1.5 m from the seat ends the replay (the distance is an emulator threshold).

### Gardens and interactive decorations (`PC2S_StartInteractionAnimReq` 20069, `PC2S_ExecuteInteractionReq` 20070, `PC2S_EndInteractionAnimReq` 20071)

Garden plots (doodads 160066, 160068, 160069), decoration 161124 and the preset farm object
180005 change phase when used. `tools/build_housing_data.py` exports their
`DoodadPhaseFlow` graphs to `data/generated/housing/doodads.json`: a phase holds interaction
transitions (one per seed for an empty plot) and at most one timer, with weighted next phases,
item conditions (`ConsumeItem` seeds, `OwnItem` trophies) and rewards (`ExpertiseReward` or
`LootingPack` loot packs, joined like gathering). Montage timings use the earliest execute
across character rigs. One transition (phase 52's pest removal without a next phase) cannot be
honoured and is listed under `unsupported_transitions`.

The client starts these like doodad gathering. The server requires the lot's owner (Candidate:
gardening spends and grants the owner's items), the interaction in the plot's current phase and
the item conditions. The start answers the SP flow (the same combat simulation as gathering)
and `StartInteractionAnimNoti` to the owner and the channel. The execute is accepted from the
montage's execute event (100 ms tolerance) within 30 s; it spends `ConsumeItem` items, rolls the
reward, picks the next phase and commits the lot and the owner's progress together, then
answers `ItemsUpdateNoti` (system or looting reason), `PS2C_ChangeDoodadPhaseNoti` (194) naming
the housing doodad unit ID, and `InteractionAnimRes`. A retried execute grants nothing; an end
before the execute cancels the SP drain. Static analysis confirms the client applies 194 to a
placed housing doodad (`AAC_PS2C_ChangeDoodadPhaseNoti_Handle`, 0x1461D3910).

Timed phases (growth 37-57 min, rest 2 min, decay after 5 days) advance in the one-minute
housing sweep and before every garden interaction. Phases that elapsed back to back chain in
one pass, each starting when the previous ended. Phase start times are stored as Unix ticks so
they survive restarts, and converted to the client's monotonic wire clock in `HousingAddNoti`
nubs and phase notifications. A newly placed object starts its first phase at placement.

Doors and windows: `PC2S_StartInteractionAnimReq` with `change_house_part_state` toggles one
part on a site the character may enter; viewers receive `PS2C_HousePartsUpdateRes` (203).

### Construction workshop and architect NPCs

The authored housing Construction Workshop is doodad `81020`, interaction `620`, animation
`60012`, with recipe pack `14001`. `tools/build_processing_data.py` now exports its 17 building
recipes: housing products go to `WorldProgress.housing_items`, while building drawings are
stackable recipe materials. Gold, materials and the product commit together through the
existing crafting transaction. An installed building count is preserved when crafting more
copies. Recipe learning, company-position restrictions, station identity, range, action timing,
quantity overflow and storage limits continue to fail closed.

Architects and other authored static NPCs already exist in the client scene; sending another
`UnitAddNoti` would duplicate them. `tools/build_map_static_npcs.py` builds a hash-checked,
map-scoped interaction index at `data/generated/static-npcs/definitions.json`. It uses the
verified static identity `(sector << 32) | (staticNpcId << 12) | 7`, single authored positions
and unconditional single-candidate selectors. The native session resolves dialogue and shop
targets through this index after checking its ordinary announced registry. Existing NPC
interaction-group, distance, ownership and shop-condition checks remain active. Unknown,
other-map, conditional or ambiguous actors are not authorized by guessing.

Generate both required profiles before starting the updated server:

```powershell
python tools/build_map_static_npcs.py
python tools/build_processing_data.py
```

Building drawings use the shop's normal stackable storage. Architect goods carry Blue Salt
company-level conditions (condition 13: level 3), which the shop now checks against the
character's company standing. A GM may grant the required blueprint/material
stacks for construction testing. Craft `89000006` requires blueprint `20001002` x1,
`70203001` x30, `70207001` x12 and 24,000 Gold, and produces building `2000001002`.

### Resident-list context

Character selection now supplies the owned HousingSite and, for a character saved inside a
housing area, `WorldCharacter.entry.housing_channel_id`, `housing_version` and `housing_visit`.
An omitted/zero board channel uses the current live housing channel in the requested zone,
otherwise the primary personal channel (see the NPC section above); an explicit invalid channel
remains an error. Repeated reads do not consume a pending destination or mutate a contract.

The stock resident-list UI also needs its local housing-visit context. Static analysis shows
that the normal Residence Entry List visit prepares a pending record, which the front visit
reply and HousingArea map completion commit together. A direct GM map move does not perform
that client-side preparation. Reconnect metadata is now complete, but automatic recovery of
every UI state after scene-only GM teleport remains unverified. Test normal channel entry and
reconnect separately; do not treat an empty UI without a front request as a database failure.

### Housing storage

Housing objects and buildings bought from NPC shops are kept as `WorldProgress.housing_items`
rows (quantity, installed count), sent at entry in `PS2C_WorldCharHousingItemsNoti` (131).
`PC2S_HousingItemDiscardReq` (20046) discards uninstalled storage items.

The server stores every owned copy as `quantity`, but on the wire `HousingItemCount.quantity`
is the copies still in storage (owned minus installed). The stock client keeps that value per
building or doodad, shows it in the decor list and allows a placement only while it is above
zero (`AAC_Housing_ApplyHousingItemCount`, `AAC_Housing_CanPlaceSelectedItem`, static analysis
in `dumps/ida_research/housing-storage-20261101`). Until 2026-11-01 the server sent the owned
total, so an object whose every copy was placed still appeared available; placing it again was
refused with `ItemError`, and because a save is all-or-nothing the other new objects in the
same save were lost too (live log 2026-11-01: garden plots, decorations and furniture). Every
housing-item update (saves, entry snapshot, shop, item use, crafting, mail, trade) now sends the
stored count through one helper, `housing_item_count_json` in `world/core`.

### Contract terms and expiry (`PC2S_UpdateHousingSiteContractExtensionReq` 20042 -> 85)

Monthly and periodic contracts both run 30 days. Inside the 7-day imminent window the owner may
prepay another term; `Close` stops renewal. A sweep runs about once a minute: a periodic site
renews from its owner's permits when possible, otherwise the site is packed and the lot freed.

### Move packages (`WSC2F_HousingMovePackageListReq` 20017, `PC2S_HousingMovePackage*` 20043/20044)

Packing keeps only owned objects (preset objects belong to the lot) and notifies the owner with
`PS2C_HousingMovePackageCreatedNoti` (88). A package unpacks onto an empty lot at least as large
as the original, or is recovered into housing storage.

### Guild housing (`WSC2F_GuildHousing*` 20034-20038 -> 48-55, `PS2C_GuildHousingSiteUpdateNoti` 89)

A guild master contracts a whole channel as the guild's named area (5,000 Token unlock once,
3,000 permits per term). The master or a sub-master assigns lots to members, who then own a
guild site there. Each resident needs the company position of the lot size
(`GuildHousingSite{S,M,L}positionLv`), read from the guild roster's refreshed company position.
The server's `GuildServiceDirectory` previously left that position at zero, so every assignment
was refused with `Housing_InsufficientCompanyPosition`; it now copies the roster value.
Members leaving the guild lose their guild site; a disbanded guild loses its area. Area changes
are pushed to every member's front socket. Guild grades come from `GuildService` through
`GuildServiceDirectory`, mapped explicitly between the two enums.

### GM commands (`PC2S_GmCmdReq` 20008 / `PC2S_GmCmdTextReq` 20009, game-master accounts only)

`release_housing [lot]`, `teleport_to_home`, `housing_remained_expiry_minutes <minutes>
[monthly|periodic|close]`, `contract_housing_guild <lot>`, `release_housing_guild <lot>`,
`housing_move_package_unpack <lot> <package>`, `housing_move_package_recover <packages...>`,
`housing_unlock <small|medium|large|guild> [revoke]` and `housing_account_reset`. The text
forms use the `GmCmd` field names; the structured forms use GmCmd fields 56, 74, 96, 103, 104,
115, 116, 123 and 126.

The text form also accepts the emulator-defined `housing_contract <small|medium|large>
[monthly|periodic]` (default monthly). No GmCmd field exists for a personal contract, so it has
no structured form. It calls the same `HousingService::contract` path as
`WSC2F_HousingSiteContractReq` in the first rentable zone, on the GM's session strand, and
publishes the same `CharHousingSiteUpdateNoti`, `ItemsUpdateNoti` and channel
`HousingAddNoti`. It does not send `WSF2C_HousingSiteContractRes`, because no contract window
is waiting for it.

No GM reply packet was recovered, so every housing GM command also returns chat lines that the
world session sends as private `[GM]` chat: `<command> applied.`, the rented lot and channel, a
teleport notice, or `<command> refused: <EErrorCode>` with a next step for
`Housing_NotRegisteredHousingSite`, `Housing_RentItemNotEnough` and
`Housing_CanHaveOnlyOneHousingSite`. Refusals still send the common error response as before.

## Evidence

- Opcodes: `protocol/opcodes.json` and `protocol/native-ws-opcodes.json` (wrapper vtable ID
  getters). Payload fields: `protocol/descriptors.normalized.pb`.
- Client behavior (`dumps/ida_research/housing-20261026`): housing unit IDs are
  `(lot << 12) | 5`; doodad unit IDs carry `(house_id << 14) | doodad_id` above the housing unit
  ID; nub positions are lot-local millimetres; epoch fields are 100 ns Unix ticks;
  `HousingAddNoti` builds the actor from the client's own `HousingSite` row; a visit completes
  only after both `HousingChannelVisitRes` and a `MapMoveCompleteNoti` into a HousingArea map;
  `UpdateHousingRes` with customization bytes applies only the customization.
- No official housing traffic exists in the capture corpus beyond empty entry snapshots.

## Candidate decisions and emulator policies

- Candidate: periodic contracts renew automatically at expiry; extension is allowed only inside
  the imminent window; `Close` lets the site lapse at its current expiry.
- Candidate: preset buildings and objects come with the lot and are not storage items.
- Candidate: housing charges use `ItemsUpdateNoti` reason Housing; a released lot leaves sight
  through `UnitRemoveNoti`; the doodad map key repeats the doodad unit ID's object field.
- Candidate: the housing permit (item 123) is a stackable `HousingPermit` category item.
- Candidate: visitors arrive 1 m above the target lot's sign; a channel visit without a zone key
  uses the character's current housing zone, otherwise the first rentable zone.
- Policy: 10 personal and 10 guild channels per zone, one personal site per region, at most 512
  objects per `UpdateHousingReq`, the guild assignment cooldown equals the guild-site edit
  cooldown, the expiry sweep runs once a minute, and a save is accepted up to 30 s after edit
  mode ended.
- Candidate: furniture pose packets follow the captured gathering shape, with the furniture
  as `target_unit_id`.

## Limits

- Full original-client acceptance of ordinary contracts, edit mode, construction and door toggles
  remains unverified. The local GM rental, housing scene entry and one ordinary channel-list
  visit with return succeeded. The board channel-0 repair and company-position checks have
  synthetic coverage only until a player opens Nivhen's window on the updated server.
- Renting through Nivhen needs Blue Salt company level 3 and quest 160000 acceptable, checked by
  the client's dialogue. Players who have not reached that company standing cannot see the
  rent selection; the server does not bypass that progression.
- `PC2S_ReleaseHousingReq` and `WSC2F_ReleaseHousingReq` exist only as schema classes in this
  build: static analysis found no code that constructs either message and no numeric-ID wrapper
  (`dumps/ida_research/housing-ordinary-20261029`). Front opcode 20016 belongs to
  `VAcceptQuestReq`. Players end a contract by choosing Close, which lets the site lapse at
  expiry; operators can also use the GM release command.
- The housing pull-out confirmation (`ExecuteOpenHousingPullOutResult`, confirmation 118) starts
  a generic interaction on a placed object; it is not a site release. Its follow-up interaction
  request is not traced or served by housing.
- Company positions are enforced for personal contracts and guild lot assignments. Company
  membership itself (Blue Salt) is only a client dialogue condition.
- Leaving a housing area for a recorded origin on map 8000 recreates the private tutorial
  instance; returning to a followup tutorial map or another hosted map (including the parent
  map 9012 when no origin is known) uses the general server map move without a followup
  tutorial instance. The unknown-origin parent return keeps the client-reported position and
  has no original-client observation yet.
- Mate NPC spawning on a housing lot (`PC2S_SpawnMateNpcReq.housing_lot_number`) is not tied to
  housing ownership.
- Garden limits: `ExecSkillEffect` tasks (the withering effect 12000008) are not applied;
  expertise experience from `ExpertiseReward` is recorded but not granted; the Sickle that
  harvest animations name is not required (captured doodad gathering never checked tools); only
  the lot owner may garden; timers advance at most a minute late between interactions; phase
  52's pest removal is unavailable until its plot's timer moves it on.
- No official capture covers furniture poses, garden interactions or late-viewer pose replay;
  the packet shapes follow captured gathering and the selfie replay.
- Original-client placement, reload after re-entry, seating and gardening on the updated server
  remain unverified; the save order, poses and gardens have synthetic coverage only.
- Crafting at a placed station (2026-11-02) has synthetic TLS coverage only
  (`NativeWorld.CraftingStationPlacedOnAHousingLotOpensAndCraftsOverTls`). Building add-on
  stations 169990 (Crafting Workbench) and 170000 pair a craft interaction with recipes of
  another expertise and stay refused. Original-client acceptance at a placed Kitchen is pending.
- Seedling plantings cost 5 seedlings of the chosen kind (10 on plot 160069), from
  `InteractionCondition` ConsumeItem. The client checks this itself and shows "You need
  <seedling>" without sending a request when the character holds fewer; on 2026-11-02 a
  Potato planting with 3 seedlings was refused this way while the server-side Barley plantings
  were accepted.

## Drawing purchases and construction recipes

Supported `HousingBuildingDrawing` shop goods now use stackable inventory, retaining catalog
conditions, prices and normal merchant checks. Supported station recipes consume those stacks
with their other materials and produce `HousingBuilding`/`HousingObject` quantity records in
housing storage. The revisioned craft transaction preserves `installed_count` and publishes
`ItemsUpdateNoti.housing_items`; it does not grant land ownership or place the product.

The optional map-scoped static-NPC index can authorize a client-owned architect without sending
another visual actor. The focused follow-up run passed 58 C++ tests across native housing,
static-NPC lookup, processing and merchant behavior, including blueprint purchasing,
construction consumption, installed-count preservation, overflow refusal, exact static identity
and the real exported workshop/Gyrenn mappings. The housing CTest selection passed 44 tests
with one PostgreSQL-environment-gated skip. Original-client architect, crafting and resident-list
acceptance of these changes remains unverified; see the dated follow-up in `VALIDATION.md`.
