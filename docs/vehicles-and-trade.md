# Mounts, vehicles, house mates and trade runs

This page describes the emulator's companion and trade-run services: summoned horses, the
trade wagon, house mates, trade vouchers, loading, departure, parking, selling and market
prices. It records what is implemented, the client evidence behind each rule, and which rules
are emulator choices (Candidate).

Client evidence comes from static analysis of the demo client (IDA database
`dumps/ArcheAgeChronicles.memdump.exe.i64`, indexes `dumps/ida_research/vehicle-20261026/` and
`dumps/ida_research/trade-20261026/`), with the boarding correction recorded in
`dumps/ida_research/vehicle-boarding-20261026/`, the exported CMS tables, and the client's
`tradingpost.json` map layers. No official server capture of any mount, vehicle, mate or
trade packet exists. A local-client trial showed the wagon and sent boarding requests, but
seating failed before the ID correction below; post-correction original-client driving is pending.

The 2026-10-29 attachment/lifecycle correction and its remaining client-acceptance limits are
recorded in [interaction-mount-repair-20261029.md](interaction-mount-repair-20261029.md).

## Client-compatible companion identifiers

Companion IDs occupy `0xE0000000..0xEFFFFFFF`, with a 24-bit sequence and low type nibble
2 (summoned mounts/mates) or 4 (vehicles). They remain separate from player IDs, the current
bounded NPC population and GM-spawned NPCs. The allocator skips IDs still held by live units
when the sequence wraps. Runtime companion IDs are transient; no persistence migration is needed.

The client uses only the **low 32 bits** of a wire unit ID to locate the actor for
`MountUnitNoti`, `MoveNoti` and other unit-addressed notifications (helper `0x146092DB0`).
The previous bit-62 allocation let a wagon appear but prevented those packets from finding
it. A successful boarding reply alone did not seat the player. This correction applies to
both wagons and summoned mounts and preserves owner checks and the existing packet order.

## Sources

| File | Responsibility |
| --- | --- |
| `src/data/companion_catalog.*` | `SummonNpc`, registration consumables (`registerSummonNpc` skill effects), `Vehicle`, `RepairWagon` interactions, `MateNpc`, `MateIntimateLevel`, `DoodadMateInteraction` |
| `src/world/features/companion/companion_world.*` | Shared companion units: ids, owners, seats, health, visibility, mount/dismount and move fan-out |
| `src/world/features/companion/companion_session.*` | Per-connection mount, vehicle-boarding, repair, mate and companion GM requests |
| `src/data/trade_catalog.*` | Trade goods, quality tiers, posts and their placements, purchase rewards, bonus rewards, patterns, `GameVariable` trade constants |
| `src/world/features/trade/trade_run.*` | Pure trade-run planning (load, depart, sell, cancel, park, stow), market quotes and wire builders |
| `src/world/features/trade/trade_session.*` | Per-connection trade dialogue, trade requests, wagon escape, reconnection and wagon persistence |
| `src/world/core/progress_access.*` | Store/handoff access used by both sessions for revisioned commits |
| `src/persistence/world_storage.cpp`, `migrations/001_accounts_characters.sql` (schema 19) | Persistence of registered summons, mates and the trade run |
| `src/world/features/shop/npc_shop.cpp` | Voucher crafting (`InteractUnitReq.param.voucher_group`) with trade-run rules |
| `src/world/features/hotkeys/hotkey_rest.*`, `src/world/features/hotkeys/hotkey_storage.cpp` | Front REST hot-key quick slots that feed the summon key (schema 21) |
| `src/world/session/native_world.cpp` | Routing, delivery order, world entry, exit and the movement hook |
| `src/world/features/companion/tests/companion_tests.cpp`, `src/world/session/tests/native_world_tests.cpp` | Behavioral and end-to-end tests |

## Availability

The services run for **non-tutorial sessions** only (the private map 8000 tutorial keeps its
own progress chain). They are enabled when the client catalog contains the required tables;
trade runs also read every `tradingpost.json` under
`data/generated/raw/X5/Content/Data/BuiltData/Maps`. Trading posts without a placement (the
test posts 4000, 4100 and 7000) cannot start or park wagons. The six placed posts lie on the
ArcheWorld channel maps: 1000 and 1100 on map 9012 (also 9013 for 1000), 2000 and 2100 on
9021, 3000 and 3100 on 9031. The live server can place characters only on the map of the
loaded NPC placement file (9012), so posts 1000 and 1100 are the reachable route today.

## Summoned mounts

* **Ownership.** Using a consumable whose `Item.FK_skillEffectList` names a
  `registerSummonNpc {cmsId}` effect (items 125, 134-139, 2001-2003) consumes one item,
  records the summon and answers `ItemsUpdateNoti{use, stackables}` then
  `RegisterSummonNpcRes{summon_npc_cms_id}`. A summon is registered once; another use is
  refused (`ItemUnusable`) so the client's non-deduplicating list stays correct.
  `WorldCharSummonNpcsNoti` lists the owned ids when the client starts playing.
* **Summon.** `SpawnSummonNpcReq` checks the catalog (`Summon_InvalidSummonNpcCmsId`),
  ownership (`Summon_NotRegistered`), life (`Summon_PlayerDead`) and riding state
  (`Summon_AlreadyRiding`). Success sends `SpawnSummonNpcRes{}`, then the horse's
  `UnitAddNoti` (NPC type nibble 2, `npc_cms_id = SummonNpc.FK_npcId`, raw `owner_char_id`,
  `summon`, `mount.passengers`) and `MountUnitNoti{unit, passenger, slot 0}` to the owner and
  every player who can see it. The client never asks to ride a horse; the server seats it.
* **Movement.** Mount nubs carry neutral stat 84 (`MoveSpeedRate`) = 1000 per mille, matching
  ordinary moving NPCs. The client scales the ridden character's base speed by this rate.
  `PC2S_MoveReq` has no unit id. While a player rides, its movement moves the
  ridden unit and other players receive `MoveNoti{unit_id = ridden unit}`; the rider's own
  presence move broadcast is suppressed (Candidate). Units entering or leaving the 100 m /
  120 m sight radii are announced or removed; a late viewer gets the unit with the rider
  already in `passengers`.
* **Dismount.** `GetOffUnitReq{dismount_point?}` (no typed reply) sends `MountUnitNoti`
  without `slot`, with `dismount_reason = User` and a nearby finite point, and removes the horse
  (`UnitRemoveNoti`). Removal is immediate; exact timing relative to the client's
  `HorseTable.DisMountActorRemoveDelay = 1` remains unverified. Death uses `RiderKilled`,
  and disconnect unbinds viewers before unit removal. Dead players cannot board a wagon.
* **Not sendable by this client.** `CreateOperableReq`, `ChangeSummonStateReq` and
  `DespawnSummonNpcReq` have no builder in the client and are not handled.
* **Player chat commands (emulator policy).** The client has no dismount key for horses (its
  only key-mapped get-off action is `EX5KeyAction::Tradewagon_GetOff`, F, in the wagon input
  layer); re-pressing the summon quick slot is its only horse dismount input. `!mount` and
  `!dismount` (`src/world/features/chat/player_commands.cpp`) are open to every character and
  run on the world session strand through `NativeWorldListener::player_mount`.
  `!mount` seats the lowest registered rideable summon, else lends the catalog's lowest
  rideable `SummonNpc` without registering it, and sends the same `UnitAddNoti` and
  `MountUnitNoti` as a summon but no `SpawnSummonNpcRes` (nothing was requested). `!dismount`
  is the server side of `GetOffUnitReq` without a landing point (horse or wagon), followed by
  the trade-run wagon update. Both refuse dead riders, the tutorial instance and housing
  visits, and reply privately as `[Server]`. They are server-initiated: the client's own
  riding-zone, falling and swimming prechecks do not run.
* **Not implemented.** Mount speed is the client's own movement data; no server-side speed
  validation is applied.

## Hot-key quick slots

The client starts a summon only from a quick slot (keys 5-8) that holds a `SummonNpc` id. The
slots come from two front REST routes on the lobby HTTP listener (the address
`WorldEnterRes.Granted.front_rest_address` names), established statically
(`dumps/ida_research/hotkeys-20261026/`):

| Route | Request | Response |
| --- | --- | --- |
| `GET /api/front/CharHotKeyLoadReq/<char_id>/<n>` | `Authorization: Bearer <session token>` | 200, `application/protobuf` `x5.pb.rest.CharHotKeys` |
| `PUT /api/front/CharHotKeySaveReq/<char_id>/<n>` | Bearer token, `application/protobuf` `x5.pb.rest.CharHotKey` | 200, empty body |

`<n>` is always 1 in this client and has no known meaning. A `CharHotKey` with only `slot` set
clears that slot. The character must be the session's selected character (403 otherwise); an
invalid session answers 401, a wrong verb 405, another content type 415 and a malformed or
out-of-range body (slots 0-63) 400. Slots persist in `emulator_character_hotkeys` (schema 21),
outside the world-progress revision, through `src/world/features/hotkeys/hotkey_storage.cpp` and `src/world/features/hotkeys/hotkey_rest.cpp`.

## Vehicles (trade wagon)

* `Vehicle` 200 is the only vehicle row (`TradeWagon`, 7 load slots, Gold 100 rental, 5,000,000
  health). It is announced as `UnitAddNoti.operable.vehicle{vehicle_cms_id, base_unit_id,
  owner_char_id, passengers}` with operable type nibble 4. The `VehicleModelTable` row names
  one `DriverSocket`, so one seat is modeled (Candidate).
* `RideUnitReq{mount_unit_id, slot}` answers `RideUnitRes{ec}` and, on success,
  `MountUnitNoti`. Errors: `Riding_TargetNotFound`, `Riding_InvalidTargetUnitType`,
  `Riding_NotRidableTarget`, `Riding_NotOwner`, `Riding_InvalidSlot`,
  `Riding_AlreadyOccupiedSlot`, `Trade_WagonTooFar` (beyond 7 m), `Summon_AlreadyRiding`.
* A driven wagon moves with its driver as above; a parked wagon stays where its driver got off.
* Repair is Interaction 62 (`RepairWagon`, `addDurability` 1500, condition target
  `DurabilityBelowThreshold` and caster `ConsumeItem {140, 1}`) through
  `InteractUnitReq{target, param.interaction_id}` on the owner's wagon: one item 140 is
  consumed, health rises up to the maximum, `ChangeStatNoti` (stat 69, `SCS_InteractionFlow`)
  reaches viewers and `InteractUnitRes` answers. An undamaged wagon or a missing kit returns
  `Interaction_ConditionsNotMatch` (the threshold value is unobserved; "below maximum" is used).
* Damage (GM `damage_vehicle`) sends `ChangeStatNoti` (`SCS_Damage`); at zero health the wagon
  breaks (`MountUnitNoti` reason `MountBroken`, `UnitRemoveNoti`) and its trade run ends
  without payment (Candidate). No world combat damages wagons yet, because combat runs only in
  the private tutorial instance.

## House mates

* `WorldCharMatesNoti` lists registered mates when the client starts playing; mates left out
  by a disconnect or crash are reset to `BeAway`.
* `SpawnMateNpcReq{mate_npc_cms_id, housing_lot_number}`: unregistered mate ->
  `SM_HOUSEMATE_SPAWN_FAIL_NOMATE`; a non-zero lot -> `SM_HOUSEMATE_SPAWN_FAIL_SPACE` (no live
  housing site ownership is wired into the world service, so mates are called outside houses,
  Candidate); a second spawned mate -> `SM_HOUSEMATE_SPAWN_FAIL_LIMIT` (one at a time, emulator
  policy). Success sends the mate's `UnitAddNoti` (`npc.mate`), `ChangedMateStateRes{Spawned}`
  and `SM_HOUSEMATE_SPAWN_SUCCESS`, and persists the state.
* `InviteMateNpcReq` -> `ChangedMateStateRes{Invited}` (`Housing_AlreadyInvitedMate` when
  repeated); `DespawnMateNpcReq` -> `ChangedMateStateRes{BeAway}` and `UnitRemoveNoti`.
  The shipped data has no interaction that sends invite/dismiss, so these two are reachable
  only through GM tools today.
* Registration (`RegisterMateNpcRes`) and intimacy (`ChangedMateIntimateRes`, capped at the sum
  of `MateIntimateLevel.nextLevelIntimate`) are granted by GM commands; no catalog item or
  effect grants a mate in this build.

## Trade runs

1. **Trade UI.** `DialogueStartReq` on an announced `TradeManager` NPC within 7 m answers
   `DialogueStartRes`. Selecting the `TradeManagement` option sends `DialogueFunctionReq`; the
   server picks the placed post nearest the NPC and ends the dialogue with `DialogueEndNoti`,
   which lets the client open the trade UI (Candidate). Trade requests require the player to
   stand within 30 m of that post's departure point.
2. **Vouchers.** Voucher crafting (`voucher_group`) charges the `MakeTradeGood` production fee
   and materials, lasts `TradeGood.validPeriod` minutes and respects
   `TradeVoucherOwnMaxLimit` (10). `WorldCharTradeVoucherNoti` lists live vouchers at entry.
3. **Loading.** `TradeGoodLoadingReq{add_goods, wagon_cms_id, start=false}` consumes vouchers
   or buys goods listed in `TradingPostGoods` (price: the production fee, else the basic value,
   Candidate) up to the wagon's 7 slots, and answers `ItemsUpdateNoti.trade_loading` (goods
   with origin post and production time; consumed vouchers as id-only entries; money change).
4. **Departure.** `start=true` also charges the wagon rental, sends `SpawnTradeWagonRes{}`, the
   wagon `UnitAddNoti` at the post's departure point and `MountUnitNoti` (the owner drives), and
   then `TradeStartNoti{trade_goods}`. A summoned horse is dismissed first. Spawn failure sends
   `SpawnTradeWagonRes{Trade_WagonSpawnFailed}` and changes nothing.
5. **Parking.** `TradingPostWagonParkingReq{post}` succeeds when the owner's wagon lies inside
   the post's async sphere (full radius; the client asks at 0.9 x radius) and answers
   `TradingPostWagonParkingRes{post}`; otherwise `Trade_InvalidPost`, `Trade_NotInProgress` or
   `Trade_PostTooFar`. `TradingPostWagonRemoveReq` stows the parked wagon (dismount with
   `ClearTrade`, `UnitRemoveNoti`); the goods stay for selling.
6. **Selling.** `TradeGoodSellReq{goods}` at the parked post (or at the origin while still
   loading) must name units the run carries. Each sold line reports quality, ratios, optional
   market event and rewards in `ItemsUpdateNoti.trade_sell{results, remaining goods}` with the
   balance changes. A sold-out run ends with `TradeEndNoti`. Reward rules (Candidate, the client
   only displays them):
   * route row with an item: `rewardItemAmount` per unit (for example 1102 from 1100 to 1000
     pays 6 x item 60007 per unit);
   * route row with money: `basicValue x rewardBasisPoints / 10000 x market ratio / 10000`;
   * origin post or no route row: `basicValue x TradeRefundBasisPoints / 10000` (10 %);
   * goods with a quality table multiply by the tier's factor (elapsed time since production);
   * `TradeBonusReward` rows roll independently per line.
7. **Market.** `TradeMarketPriceReq` -> `TradeMarketPriceRes{next_update_tick, trading_posts}`.
   The basic ratio is 100-140 % in 5 % steps, one goods group in four has an event pattern
   (`TradePatternDesc`, +30/+15/-15/-30 % by demand tier), the total is clamped to 50-180 %, and
   each window lasts `TradeMarketPriceUpdateIntervalPerMin` (60) minutes. Quotes are a
   deterministic hash of post, group and window, so every session sees the same prices
   (Candidate model).
8. **Cancel.** `TradeCancelReq` drops goods and fees (no refund, client message 173), removes the
   wagon (`ClearTrade`) and sends `TradeEndNoti`.
9. **Escape.** `EscapeReq{is_trading=true}` respects `TradeWagonRespawnCooldown` (30 s,
   `SM_TRADEWAGON_RESPAWN_COOLDOWN`), then respawns the wagon beside its owner with its health
   and the owner aboard, answering `SpawnTradeWagonRes{}` (Candidate: the client describes a
   "previous route point"; trade-road route points are not used).
10. **Reconnection.** The wagon's transform and health are saved when the owner gets off, parks
    or disconnects. At the next entry the wagon reappears there with `TradeWagonPosNoti`.
    `TradeStartNoti` follows at entry only when the owner is already driving the wagon (a
    region re-entry). An owner who enters on foot receives it when it next boards the wagon
    (`RideUnitReq`) or when trade escape seats it, after `MountUnitNoti`. Static client
    analysis shows why: `AAC_Trade_FX5TradeManager_OnTradeStartNoti` switches the screen to
    opaque black (linear colors `(0,0,0,0)` to `(0,0,0,1)`, duration 0), and the 0.5 s trade
    tick restores it only while the local character rides a vehicle. Sending it to an owner on
    foot left the world black after login while the game kept running (observed live on
    2026-11-01). Loading and stowed runs send no `TradeStartNoti` at entry; their client trade
    state stays idle (Candidate: whether selling a stowed run's goods needs a non-idle client
    state is not verified). `TradeWagonPosNoti` is also sent when the owner gets off.

Not implemented: `TradeEventResultNoti` and `RemoteTradeVehicleRes` (ignored by this client),
random encounter events (`TradeGood.FK_randomEncEvent` has no client table), daily trade
permits (messages 260/261 have no table), `OnDemandDynamicCharTradeCompletionsNoti`, and the
marketplace (`Market*` packets), which belongs to a separate auction feature. Voucher company
conditions now read the character's company standing and require a valid pioneer permit.

## Persistence (schema 19)

`emulator_character_summons` (one row per registered summon, never deleted),
`emulator_character_mates` (state, lot, intimacy) and `emulator_character_trade_runs` (phase,
posts, vehicle, start time, goods JSON bounded to 64 units, wagon transform and health, last
spawn) are written in the same revisioned transaction as gold, stacks, vouchers and items.
A stale revision writes nothing.

## GM commands

Text console (`GmCmdTextReq`) and structured `GmCmdReq` fields, game-master accounts only:
`register_summon_npc <id>`, `spawn_summon_npc <id>`, `despawn_summon_npc`,
`register_mate_npc <id>`, `add_mate_intimate <id> <value>`, `spawn_mate_npc <id>`,
`despawn_mate_npc <unit>`, `invite_mate_npc <unit>`, `spawn_operable <vehicle>`,
`damage_vehicle <amount>` (own wagon) and `trade_voucher_add <good> [minutes]`.

## Verification

See `VALIDATION.md` ("Mounts, vehicles, mates and trade runs — boarding correction"). The
Release vehicle suite passed 31 cases with two PostgreSQL-gated skips; the synthetic TLS
summon and GM-wagon boarding/driving/dismount/reboarding tests also passed. A full main-suite
snapshot passed 803 of 865, skipped 60 and failed two: the previously documented bag-unequip
access violation and the trade-route fixture whose merchant is absent from the current NPC
placement export. This is not a full-suite pass. Earlier storage tests remain historical
persistence evidence, not checks rerun by the boarding correction. The failed pre-correction
local-client trial and the static dispatcher finding are distinct from these synthetic checks.
Post-correction original-client driving, braking and dismount acceptance remain pending.
