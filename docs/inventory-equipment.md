# Inventory, equipment, and currency protocol

Research phase 1 notes (2026-10-24). Scope: world-path item messages for the
native client. Not covered: housing storage, market/trade, mail attachments
(same `Item`/`MoneyChange` primitives; separate checkpoint).

Client build: `hotfix-26.10.18.734`; dumped image SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`
(`dumps/ArcheAgeChronicles.memdump.exe`, image base `0x140000000`).

Provenance key:

- `schema` — `protocol/descriptors.normalized.pb` field layout.
- `opcodes` — `protocol/opcodes.json` (IDA-established wrapper getters,
  `static_verified`: vtable slot + exact UTF-16 packet name).
- `cap` — observed frame in `out/captures/*/decoded.jsonl`.
- `lp` — observed frame in
  `G:\Dumps\Archeage\ClientData_2026-10-23\LivePackets\Decoded\packets.decoded.jsonl`
  (17,055-frame official corpus).
- `catalog` — `data/generated/catalog/Tables/*.jsonl` (hash-verified export).
- `ida` — named/wrapped function in the `.i64`; candidate otherwise.

Confidence: **Observed** = captured official frame decoded; **Schema** =
recovered descriptor + opcode registration, no live frame; **Candidate** =
interpretation, needs a live observation or finished IDA analysis.

## Client → server (PC2S, opcodes 20000+)

| Packet | Op | Schema (x5.pb) | Observed? | Provenance |
| --- | --- | --- | --- | --- |
| `PC2S_ItemDiscardReq` | 20072 | `repeated ItemIdentity identity_list{item_id, ItemCountInfo stackable{item_cms_id,count}, trade_voucher_id}` | no | schema+opcodes |
| `PC2S_ItemDisassembleReq` | 20073 | `repeated sfixed64 item_ids` | no | schema+opcodes |
| `PC2S_ItemEquipReq` | 20074 | `EX5EquipSlotType slot=1; sfixed64 item_id=2; PacketSeq packet_seq=15` | yes (4 frames: 3 lp, 1 local cap) | schema+opcodes+cap/lp |
| `PC2S_ItemPolishReq` | 20075 | `sfixed64 item_id; int32 add_material_item_cms_id` | yes (7 exchanges, `item_id` only) | schema+opcodes+cap; handled, see inventory-polish-evidence.md |
| `PC2S_ItemLvUpExpertiseReq` | 20076 | `EX5ExpertiseType expertise_type; int32 item_cms_id` | no | schema+opcodes |
| `PC2S_ItemUseReq` | 20077 | `int32 item_cms_id; sfixed64 item_id; int32 count; uint64 target_unit_id; bool use_quick_slot; PacketSeq` | no | schema+opcodes |
| `PC2S_WeaponSlotActivationReq` | 20078 | `EX5EquipSlotType slot` | no | schema+opcodes |
| `PC2S_LootItemReq` | 20079 | `uint64 unit_id; PacketSeq` | yes (36 lp+cap) | schema+opcodes+cap/lp |
| `PC2S_UtilityBagSetReq` | 20080 | `repeated UtilityBagSlot{uint32 slot; int32 item_cms_id; int32 count}` | yes (6) | schema+opcodes+cap/lp |
| `PC2S_ChannelEventItemTributeReq` | 20081 | (not decoded) | no | schema+opcodes |

## Server → client (PS2C)

| Packet | Op | Schema | Observed? | Provenance |
| --- | --- | --- | --- | --- |
| `PS2C_LootItemRes` | 26 | `EErrorCode ec; uint64 unit_id; PacketSeq` | yes (36) | schema+opcodes+cap/lp |
| `PS2C_UpdateWeaponDropStateNoti` | 63 | `unit_id; item_id; tick; EX5EquipSlotType weapon_slot; bool dropped` | no | schema+opcodes |
| `PS2C_ChangeWeaponSkillSlotNoti` | 96 | `skill_effect_cms_id; expire_tick` | no | schema+opcodes |
| `PS2C_UtilityBagSetRes` | 104 | `EErrorCode ec; UtilityBag{item_cms_id1..4,count1..4}` | yes (7) | schema+opcodes+cap/lp |
| `PS2C_WorldCharMoneyNoti` | 126 | `bool update; DbCharacterMoney{map<int32,int64> values}; int64 blue_salt` | yes (6) | schema+opcodes+cap/lp |
| `PS2C_WorldCharItemsNoti` | 130 | `EX5EquipSlotType weapon_slot; repeated Item; repeated ItemStackable; bool update; bool has_more` | yes (3) | schema+opcodes+cap/lp |
| `PS2C_WorldCharHousingItemsNoti` | 131 | (housing) | yes (3, empty) | schema+opcodes+cap/lp |
| `PS2C_WorldCharItemCraftsNoti` | 132 | (crafts) | yes (3, empty) | schema+opcodes+cap/lp |
| `PS2C_WorldCharUtilityBagNoti` | 140 | `UtilityBag bag` | yes (3) | schema+opcodes+cap/lp |
| `PS2C_OnDemandDynamicCharItemStorageNoti` | 148 | `repeated Item; repeated ItemStackable` | no | schema+opcodes |
| `PS2C_EquipmentsUpdateNoti` | 168 | `PacketSeq; uint64 unit_id; repeated Equipment{item_cms_id; grade; slot}; uint32 equip_slot_types` | yes (55) | schema+opcodes+cap/lp |
| `PS2C_ItemsUpdateNoti` | 169 | `repeated Item; repeated ItemStackable; repeated MoneyChange{type;change;result}; repeated HousingItemCount; PacketSeq; reason fields 11-43` | yes (369) | schema+opcodes+cap/lp |
| `PS2C_ItemStorageStoreRes` | 170 | `repeated Change{item_id; bool store}; repeated ItemStackableStore{item_cms_id;quantity;store_quantity}` | no | schema+opcodes |
| `PS2C_ShopItemLimitNoti` | 171 | `shop_id; repeated ShopItemLimit{item_cms_id;count;reset_epochtime}` | yes (1) | schema+opcodes+lp |
| `PS2C_WeaponSlotActivationNoti` | 172 | `EErrorCode ec; uint64 unit_id; EX5EquipSlotType slot; sfixed64 activation_delay_tick` | yes (7) | schema+opcodes+cap |
| `PS2C_UnitLootItemInfoNoti` | 173 | `uint64 unit_id; bool possible_handling_loot; bool has_chance_to_loot` | yes (64) | schema+opcodes+cap/lp |

## Shared primitives

```
Item { item_id sfixed64=1; item_cms_id int32=2; grade EX5ItemGrade=3;
       bind EX5BindType=4; durability int32=5; equip_slot EX5EquipSlotType=6;
       polish_step=7; created_epochtime=8; expiry_epochtime=9;
       option_cms_id=10; ItemOption options[11]; ItemSocket sockets[12];
       Stat stats[13]; }
ItemStackable { item_cms_id int32=1; quantity int32=2; }
Equipment     { item_cms_id; grade; slot }          # broadcast visual form
MoneyChange   { EX5MoneyType type=1; int64 change=2; int64 result=3; }
DbCharacterMoney { map<int32,int64> values }        # key = EX5MoneyType ordinal
PacketSeq     { uint64 seq=1 }
EX5EquipSlotType: Weapon_1..Tool_4 (0-5), Costume* (6-10,17,18),
                  Necklace..Ring (11-16), Armor* (19-22), FlightTool 23, Bag 24
EX5ItemGrade: Normal..Mythic (0-5)
EX5BindType: None/Account/Character (0-2)
EX5MoneyType: Gold=0, HersanderToken=1
```

## Semantics

### Equip / unequip (Observed)

- Client sends `ItemEquipReq{item_id, [slot]}`. Slot is omitted for a weapon
  equip (lp seq 3951, cap) and present for `CostumeSuit`/`FlightTool` equips.
- Server replies `ItemsUpdateNoti{reason.equip, items:[...]}` listing **both**
  sides of the swap: newly equipped item carries `equip_slot`; the vacated item
  is reported with `equip_slot` **absent** (observed lp t=981844, 4236724). No
  sentinel/negative value; "absent field" = unequipped.
- No separate Unequip/ItemMove request exists in the descriptor set or opcodes.
  Unequip is presumably `ItemEquipReq` for a slot whose occupant moves out
  (Candidate: whether unequip sends `slot` alone or `slot`+different `item_id`
  needs one more capture or finished handler analysis).
- `EquipmentsUpdateNoti` broadcasts the visual equipment of any unit: an
  `Equipment` entry with `slot` but no `item_cms_id` clears the slot
  (observed: Costume*/Armor clears during NPC/player changes).
- `weapon_slot` on `WorldCharItemsNoti` marks the active weapon for stats;
  `equip_slot` 0 vs 1 (`Weapon_1`/`Weapon_2`) are the physical pair. Only one
  weapon contributes to stats (see character-stats.md).

### Move / swap / split / merge (Schema → likely client-local)

No PC2S message carries an inventory position, destination slot, or count to
move/split. `Item.equip_slot` is the only positional field on the wire;
`WorldCharItemsNoti.items`/`stackables` are unordered sets (map iteration
order observed). Inventory grid ordering, stack split and merge therefore
appear client-side only, or folded into server ops not yet observed. Marking
**Candidate**; confirm via handler analysis after IDA auto-analysis finishes.

## Sanitized equip fixture

Official-corpus equip/unequip sequences with runtime identifiers removed
(`item_id` → stable letters, `unit_id`/`created_epochtime` stripped, sequence
numbers dropped). Source:
`LivePackets/Decoded/packets.decoded.jsonl` (official, `static_verified`
decode). `<unset>` = proto3 field absent on the wire.

```
t≈981824  PC2S_ItemEquipReq            {item_id:B}                        # slot omitted
t≈981844  PS2C_ItemsUpdateNoti.equip   items:[{A, cms 11104102, dura 9744, equip_slot:<unset>},
                                             {B, cms 13104202, Grand, dura 10000, equip_slot:Weapon_1}]
t≈981844  PS2C_EquipmentsUpdateNoti    unit:SELF equipments:[{cms 13104202, Grand, slot:<unset>=0=Weapon_1}]

t≈4236705 PC2S_ItemEquipReq            {slot:CostumeSuit, item_id:C}
t≈4236724 PS2C_ItemsUpdateNoti.equip   items:[{D, cms 10130402, dura 10000, equip_slot:<unset>},
                                             {C, cms 10140231, dura 10000, equip_slot:CostumeSuit}]
t≈4236768 PS2C_EquipmentsUpdateNoti    unit:SELF equipments:[{cms 10140231, slot:CostumeSuit}]

t≈4244591 PC2S_ItemEquipReq            {slot:FlightTool, item_id:E}
t≈4244610 PS2C_ItemsUpdateNoti.equip   items:[{E, cms 11600103, Grand, dura 10000, equip_slot:FlightTool}]
t≈4244611 PS2C_EquipmentsUpdateNoti    unit:SELF equipments:[{cms 11600103, Grand, slot:FlightTool}]

# local capture out/captures/local-pickup-20261024-01 (own client)
PC2S_ItemEquipReq  frame hex 57ba0e0011006a4e39 0f000011c001c0a4
#  header 9B: magic/len/opcode(0x4e6a=20074)/transport-seq
#  body 8B: tag 0x11 (field2 sfixed64) + LE item_id — no slot field, no packet_seq
```

Established by the fixture:

- Equip and slot-swap share one request; `slot` is sent only when the target
  slot is not the request's default (weapon equip omits it; CostumeSuit/
  FlightTool equips send it).
- `ItemsUpdateNoti.items` entries carry **no `packet_seq` value** (field
  present-but-empty = seq 0): server does not echo request sequencing here.
- `EquipmentsUpdateNoti` on the same tick broadcasts the visual change for
  `unit:SELF`; `slot` absent when it equals enum default 0 (`Weapon_1`), so
  absent `slot` is `Weapon_1`, not "unspecified".
- Unequip-without-replacement is **not** covered by any observed frame:
  every observed equip moved another item out. Whether the client sends
  `ItemEquipReq{slot}` with `item_id` empty/0 for a bare unequip remains
  **Candidate** pending the request-builder analysis (open question 1).

### Delete / drop (Schema only)

`ItemDiscardReq` carries repeated `ItemIdentity{item_id, stackable count,
trade_voucher_id}` — one request discards equipment and stackables.
`Item.disassemble` requires unequipped items per error codes. Tempering
(`ItemPolish_RequireUnEquip`) does not: captured equipped weapons were tempered;
the server only requires unequipping when the step can destroy the item.
No discard/drop frame observed in either corpus.

### Use (Observed for item skills; schema only for ItemUseReq)

Consumables with a catalog skill go through `StartSkillReq{skill_id, use_item}` (potion
40101101 → catalog skill 1000, consumed at 435 ms). All 54 official item uses in the capture
corpus use this path; `ItemUseReq` (20077) was never observed. The server's item validation,
consumption and effects are described in [item-use.md](item-use.md).

### Capacity, bag, weight, max stack (Catalog + schema)

- `Item.weight` per unit; capacity is weight-based.
  `GameVariable`: `BagDefaultSafeWeight=50000`, `BagMaxWeightMultiplier=200`,
  `StorageDefaultWeight=100000`, `StatEquipWeightValue=150` (weight per stat),
  `ItemMaxStack=1000`.
- Equipping a `Bag`-slot item (`Item.equipSlot=Bag`, e.g. 11700201 starter bag)
  raises safe weight via `Item.safeWeight` (135000 for the starter bag; up to
  600000 on 13700205). This is the bag-expansion mechanism — no packet expands
  slots; capacity derives from the equipped bag item + stats.
- Encumbrance: `StatusEffectIdForInvWeightHeavy=50030004`,
  `...VeryHeavy=50030005`, `InvWeightMoveSpeedStep2RatioFrom=0.5`,
  `EquipWeight*Ratio` constants define thresholds.
- `x5.cms.Storage{slot_limit=10, weight_limit=1000}` /
  `EX5StorageType_Normal` + `StorageType.rejectedItemCategories=Equipment`
  looks like a generic container profile (housing/storage box), not the
  character bag — **Candidate** mapping.
- Result codes for capacity: `EErrorCode` 1004 `ItemWeightLimit`,
  1005 `ItemSlotLimit`, 1024 `ItemNotEnoughFreeWeight`, 900 `WeightPenaltyState`.
- Max stack: `ItemMaxStack=1000` (catalog GameVariable). No per-item override
  observed in `Item` fields; `ItemLimit{looting_amount_limit}` caps daily/
  monthly looting for specific items.

### Currency (Observed)

- `EX5MoneyType`: `Gold=0`, `HersanderToken=1` (virtual item flag set in
  `Money.jsonl`). Both limited to `limitUserMax=9e15`.
- `blue_salt` (premium/paid-shop currency) is a separate `int64` on
  `WorldCharMoneyNoti` — **not** part of `DbCharacterMoney.values`.
- Bootstrap: `WorldCharMoneyNoti{update:false, money.values{0:gold}}` observed
  at entry with gold only; a second `update:true` frame carried `blue_salt`.
- Mutations have **no dedicated money packet**: `ItemsUpdateNoti.money_changes`
  carries `MoneyChange{type, change, result}` (result = new balance). Observed
  reasons carrying money deltas: `looting`, `adventure_object_activate`,
  `shop`, `system`, `death` (-35 on death, lp t=1199244).

### Loot (Observed)

Flow: `UnitLootItemInfoNoti{has_chance_to_loot}` on kill → client
`LootItemReq{unit_id}` → `LootItemRes{ec, unit_id}` + `UnitLootItemInfoNoti`
(cleared flags) + `ItemsUpdateNoti{reason.looting{target_unit_id}}` delivering
money and stackables. Error codes 6001-6008 cover invalid/empty/over-weight
loot (`Npc_LootOverMaxWeight`).

Detail from the 2026-10-24 captures (full ordering, corpse lifetimes, item rows and the
server design are in [monster-loot.md](monster-loot.md)): the kill-time
`ItemsUpdateNoti.looting` carries only a gold `money_changes` entry, the take-time one carries
`items`/`stackables` (resulting totals), and `Action_InvalidLootingRange` is the only observed
refusal.

### Utility bag / quick slots (Observed)

`UtilityBagSetReq{slots[]}` → `UtilityBagSetRes{ec, UtilityBag}` +
`ItemsUpdateNoti{reason.utility_bag}` count sync + `WorldCharUtilityBagNoti`
at entry. Errors 17000-17008 cover combat/slot/group limits.
`Item.FK_utilityBagFullFillCnt` + `utilityBagGroup` (catalog) constrain
allocations; `UtilityBagFullFillCnt` rows define per-group max counts.

#### Emulator implementation (2026-10-27)

The four quick item slots (HUD keys 1-4) are `WorldProgress::utility_slots`, persisted in
`emulator_character_utility_slots`. Each slot reserves owned stack units; it holds no items.

- `src/world/features/inventory/utility_bag.cpp` owns the request rules for both sessions: at
  most four entries for slots 0-3, applied in order so a slot named again takes its last entry,
  non-negative counts, an empty slot has no count, an assigned item must be bag-eligible
  (catalog `Item.FK_utilityBagFullFillCnt`; the shared world reads the client catalog's `Item`
  table, the tutorial the same rule exported as its profile's `utility_items`), and all slots
  together may reserve at most the owned units of each stack after the last entry applies.
  Malformed or ineligible slots answer `CommonErrorRes{BadRequest}`; over-reservation answers
  `CommonErrorRes{Interaction_ConditionsNotMatch}`. These are the tutorial's existing local
  codes; the official 17000-17008 `UtilityBag_*` refusals are not reproduced.
- Tutorial (map 8000/10117): `TutorialSession` applies the request and sends
  `WorldCharUtilityBagNoti` (140) in its time-sync snapshot, as before.
- Shared world: `WorldSession` commits `UtilityBagSetReq` through the progress revision, answers
  `UtilityBagSetRes` (104) with the saved bag, and, when the saved bag has any assigned slot,
  sends 140 once per connection after the quest snapshot at the first time sync. An empty bag
  is the client's initial state and is not announced, which also keeps the established
  shared-world entry frame order unchanged for characters without a bag.
- `ItemUseReq` spends (draw boxes, effect consumables) lower slot reservations that the
  remaining stack no longer covers and then send 104 with the committed bag (Candidate: no
  official frame shows how the client learns a lowered count on this path).

Hosted-server diagnosis that motivated the shared-world path: a character set its bag inside
the tutorial (four accepted 20080 requests), completed the tutorial and was saved on map 9012.
Its six later shared-world entries received no 140, although the database still held the
saved slots, so the HUD showed empty slots after every rejoin. Shared-world 20080 requests
were also ignored. Potions in a restored slot still cannot be drunk outside the tutorial,
because item skills (`StartSkillReq{use_item}`) are tutorial-only.

#### Repeated slot entries (2026-10-30)

A player re-equipping a potion that was already in the quick slot got "Invalid Request". The
hosted log showed two shared-world 20080 requests refused with `BadRequest`; the saved bag
rows held assigned potion slots whose reservation had been spent to zero while the owned stack
still had units, so re-equipping is how a player refills such a slot.

Client sender `AAC_UtilityBag_SetReq_SendSlotAssignment` (0x145F67E30, saved IDB) builds the
request from the current local bag. When the assigned item, or an item of the same
`utilityBagGroup`, already occupies slot `S` and the target is slot `T`:

1. if `S != T`, it adds `{slot S}` (cleared);
2. if `T` currently holds an item, it adds `{slot T}` and then `{slot T, its current item and
   count}` (both through `RepeatedPtrField::Add`, helper 0x145F68750);
3. it always adds `{slot T, assigned item, full effective fill count}`.

Re-equipping into the same slot therefore names slot `T` three times, and the server's former
"unique slots" rule refused it. Requests now apply entries in order, a repeated slot takes its
last entry, and ownership is checked after all entries. The four-entry limit still covers the
largest client shape (steps 1-3 with `S != T`). The official server's handling of these
intermediate entries is unobserved; the six captured requests are counted, not decoded here.

### Equip result codes (Schema)

`EErrorCode` 1000-1055: `ItemError`, `ItemCmsIdInvalid`, `ItemIdInvalid`,
`ItemBagInvalid`, `ItemWeightLimit`, `ItemSlotLimit`, `ItemCountError`,
`ItemUnusable`, `ItemHasExpired`, `ItemEquipInvalidSlot`,
`ItemEquipInvalidItemType`, `ItemEquipBroken`, `ItemNonDiscardable`,
`ItemNonSellable`, `ItemEquipped`, `ItemNotEquipped`, `ItemLocked`,
`ItemClassMismatch`, `ItemEquipInvalidState_{Combat,Dead,ClassActionForm}`,
`Disassemble_*`, `ItemPolish_*`, `Weapon_AlreadyActivated`,
`Weapon_ActivationNotDelayTickYet`, `Weapon_InvalidSlot`. Money: 1201
`MoneyNotEnough`, 1202 `PurchaseLimitExceeded`.

`WeaponSlotActivationNoti.activation_delay_tick` observed ≈ +2 s vs swap;
`EquipWeaponSwapDelay=1000` (ms) in GameVariable — tick units need confirmation
(observed delta ~2 s suggests 500 000 000 ticks/s native clock).

## Implementation spec — step 2 (money + capacity)

### Gold today

- `native_world.cpp` loads `WorldProgress` for a non-tutorial handoff, queues
  `WorldCharItemsNoti` (130) with persisted stackables, then queues
  `WorldCharMoneyNoti` (126) at map move. It sends `update:false` and
  `money.values["0"]` as the persisted gold balance. Tutorial entry remains
  owned by `TutorialSession::snapshot()`, without duplicate notifications.
- Gold is authoritative in `WorldProgress.gold` /
  `emulator_characters.gold` (`CHECK gold >= 0`); commits already ride
  `save_world_progress` revision CAS with item/stackable writes.
- Mutations: emit `ItemsUpdateNoti.money_changes[]` entries of form
  `{money_type:<unset or 0>, change:<delta>, result:<new_balance>}`.
  `money_type` absent in every observed gold frame → field 1 default (Gold).
  Sanitized observed deltas (official corpus):

  | Reason | change | result | Note |
  | --- | --- | --- | --- |
  | looting | +6 | 6 | first gold of session; result starts 0 |
  | looting | +6 | 49 | |
  | looting | +9 | 64 | |
  | looting | +9 | 164 | |
  | looting | +90 | 254 | |
  | looting | +90 | 260/362 | two stacked kills |
  | looting | +338 | 700 | larger reward |
  | adventure_object_activate | +37 | 43 | floor pickup gold |
  | adventure_object_activate | +91 | 155 | floor pickup gold |
  | shop | -10 | 690 | purchase |
  | death | -35 | 1865 | death penalty deduction |
  | system | +200 | 1890 | server grant |
  | system | +1000 | 1690 | grant bundled with an item row |

  Invariant: `result = previous_balance + change` in every observed frame —
  the server must send the post-write balance, not the pre-write one.
- Cap: `Money.limitUserMax = limitSystemMax = 9 000 000 000 000 000`
  (Gold and HersanderToken). Clamp grants server-side; no observed error code
  for overflow (`MoneyNotEnough=1201` only covers spends).
- `blue_salt` is a separate `int64` on 126 (sent once as `update:true` at
  entry); not in `DbCharacterMoney.values`. Persistence would need a new
  column; skip in step 2 unless required.

### Capacity inputs (catalog, hash-verified)

```
Item.weight           per-unit weight (int; observed 1–8800, e.g. potion 10,
                      staff 1500, armor suit 4800, bag itself 100)
Item.safeWeight       present on Bag-slot items → capacity bonus
                      (11700201 starter bag: 135000; range 200000–600000 on
                      higher bags; GM item 99: 1000000000)
BagDefaultSafeWeight  50000   (baseline capacity without a bag)
BagMaxWeightMultiplier 200    (hard ceiling = safe * multiplier?)
StorageDefaultWeight  100000  (non-character storage default)
StatEquipWeightValue  150     (weight capacity per stat point — which stat
                              unconfirmed; Candidate)
ItemMaxStack          1000    (GameVariable; per-stack quantity cap)
```

Derived check: every starter kit fits trivially under the starter bag —
total kit weight including the bag item:
Fight 1800, Wild 1300, Adamant 2300, Magic 15300
vs `safeWeight` 135000 (starter bag). Whether `weight` sums only
unequipped items or all carried items needs one over-capacity observation —
**Candidate**: equipped gear likely still weighs (armor rows have weight).

### Error-code selection (schema enum; no observed failure frames)

| Condition | Code | Evidence |
| --- | --- | --- |
| New item would exceed weight capacity | `ItemWeightLimit` 1004 or `ItemNotEnoughFreeWeight` 1024 | names only; exact split Candidate (1024 likely loot/grant path, 1004 equip/bag ops) |
| Slot-count capacity overflow | `ItemSlotLimit` 1005 | `Storage.slot_limit` exists for containers; whether the char bag has a slot cap unconfirmed (no grid positions on wire → probably weight-only for character inventory) |
| Stack quantity > cap / count mismatch | `ItemCountError` 1006 | clamp at `ItemMaxStack` 1000 |
| Loot overweight | `Npc_LootOverMaxWeight` 6006 | loot path |
| Carrying-encumbrance thresholds (no reject; status effects instead) | — | `StatusEffectIdForInvWeightHeavy/VeryHeavy` 50030004/5, `InvWeightMoveSpeedStep2RatioFrom` 0.5; `WeightPenaltyState` 900 is a state code, not a transaction error |
| Gold spend insufficient | `MoneyNotEnough` 1201 | quest accept requirement + shop paths |

### `ItemDiscardReq` server validation (schema + error taxonomy; no frame observed)

Request: `repeated ItemIdentity{item_id, stackable{item_cms_id,count}, trade_voucher_id}`.
Expected checks, marked by basis:

| Rule | Code | Basis |
| --- | --- | --- |
| `item_id` exists and belongs to requesting character | `ItemIdInvalid` 1002 / `ItemError` 1000 | schema |
| `stackable.count` ≤ owned quantity, >0 | `ItemCountError` 1006 | schema |
| `Item.discardable == false` → reject | `ItemNonDiscardable` 1012 | catalog `discardable` field (QuestItem/key rows are False) |
| Equipped item → reject | `ItemEquipped` 1016 | enum |
| Item locked (bind/lock state) | `ItemLocked` 1018 | enum; bind types exist but no "locked" wire flag observed → Candidate how lock is represented |
| Success → `ItemsUpdateNoti{reason.discard{…}}` with updated `items`/`stackables`; `TradeVoucher` refunds may appear in `Reason_Discard.vouchers` | — | schema of reason field |
| Repeatable batch = all-or-nothing? | — | **Candidate**; safest is validate-all-then-commit atomically under revision |

### Exact error-code values

Source: `protocol/descriptors.normalized.pb`, `x5.ec.EErrorCode` in
`ec.generated.proto`. Names indicate categories; assignment to a particular
failure path remains Candidate without a captured error or verified handler.

| EErrorCode name | Value |
| --- | ---: |
| `ItemIdInvalid` | 1002 |
| `ItemWeightLimit` | 1004 |
| `ItemSlotLimit` | 1005 |
| `ItemCountError` | 1006 |
| `ItemEquipInvalidSlot` | 1009 |
| `ItemNonDiscardable` | 1012 |
| `ItemEquipped` | 1016 |
| `ItemLocked` | 1018 |
| `ItemClassMismatch` | 1019 |
| `ItemNotEnoughFreeWeight` | 1024 |
| `MoneyNotEnough` | 1201 |

`1014` is **`ItemNotRepairable`**, not `ItemEquipped` or a discard code.

### Step-2 open gaps

- Whether bag contents count equipped-item `weight` toward `safeWeight`.
- `ItemSlotLimit` applicability to character inventory (no grid on wire).
- `ItemLock` representation (no field seen; possibly `bind` + server state).
- HersanderToken wire presence (values key 1) — paid content, unobserved.

## Existing server state to reuse

| Layer | What exists |
| --- | --- |
| `src/persistence/character_state_types.hpp` `CharacterItem` | `id, cms_id, inventory_slot, equip_slot?, quantity, durability, grade` — maps `Item`; DB table `emulator_character_items` (UNIQUE inventory_slot, partial UNIQUE equip_slot). |
| `src/persistence/character_state_types.hpp` `WorldProgress` | `revision, gold, experience, stackables{cms:qty}, adventure_objects, quests, doodad_phases, utility_slots` — tutorial money/stacks already persist via `world_storage.cpp` compare-and-commit with gameplay_revision. |
| `src/world/session/native_world.cpp` | sends `WorldCharItemsNoti(130)` from `character_inventory()` with persisted `WorldProgress.stackables` on non-tutorial handoffs, then `WorldCharMoneyNoti(126)` with persisted gold. `character_equipment()` feeds `PlayerNub.equipments`. Tutorial snapshots remain owned by `TutorialSession`. |
| `src/world/features/tutorial/tutorial.cpp` | emits 169 `ItemsUpdateNoti` stackable/money deltas with reason objects, `UtilityBagSetRes(104)`, `WorldCharMoneyNoti(126)`, `WorldCharUtilityBagNoti(140)` — correct wire shape already proven by verify_packets against descriptors. |
| `src/data/starter_gear.hpp` | per-class starter `CharacterItem` sets; class weapons fix landed for the char-creation bug. |
| `migrations/001_accounts_characters.sql` | `emulator_character_items`, `emulator_character_stackables`, `gold` column on `emulator_characters`. No schema change needed for item/money persistence; `blue_salt`/HersanderToken would need a new column or `values` table if implemented. |

## IDA findings so far

- All item-family opcode/name getter wrappers already renamed `AAC_*`
  (`applied-results.tsv`, e.g. `AAC_PC2S_ItemEquipReq_GetOpcode` @
  `0x145EC9AD0`, `AAC_PS2C_ItemsUpdateNoti_GetOpcode` @ `0x1461BE710`,
  `AAC_PC2S_ItemDiscardReq_GetOpcode` @ `0x1461923F0`,
  `AAC_PC2S_ItemUseReq_GetOpcode` @ `0x145EF0C50`,
  `AAC_PS2C_EquipmentsUpdateNoti_GetOpcode` @ `0x144C08480`,
  `AAC_PS2C_WorldCharItemsNoti/MoneyNoti_GetOpcode` @ `0x1461BEBA0/C0`).
- Client-side **request dispatch** (where UI builds `ItemEquipReq` / decides
  `slot` omission for unequip) NOT yet located. `analysis_status` on instance
  `k7ui` (checkpoint-4 probe, 2026-10-24): `queue_empty=false`,
  `finished=false`, `function_count=705878`, state "queued (paused while
  servicing this request)" — unchanged from the earlier probe; code xrefs
  still incomplete, so request-builder attribution is deferred. Per policy a
  single probe was made and no IDA mutations performed.

## Open questions / next bounded steps

1. Unequip request encoding: `ItemEquipReq` with `slot` and empty/other
   `item_id`, or implicit? Needs finished IDA xrefs on the request builder
   (vtable `0x14A7955C0`) or a live unequip capture.
2. Does the client ever send `ItemUseReq` (20077), or do all consumables use
   `StartSkillReq`? Needs equip/consume capture outside tutorial.
3. `ItemDiscardReq` semantics for partial-stack discard (`stackable.count`)
   and whether discard of equipped item is rejected client-side.
4. `WeaponSlotActivationNoti` tick units and `weapon_state` transitions for
   Weapon_1/Weapon_2 swaps (`EX5WeaponStateType` strings in PlayerNub).
5. Inventory ordering/grid: confirm no wire order (client-local arrangement)
   vs an unobserved position field.
6. `PacketSeq.seq`: request/response correlation — `packet_seq` observed
   present-but-empty in most `ItemsUpdateNoti`/`LootItemRes`; which requests
   echo `seq` back?
7. `blue_salt` writes and `HersanderToken` wire presence (values[1]) — needs
   paid-shop flow; persistence needs a new column if implemented.
8. `Item.equip_slot` semantics for items in `ItemIntoSocket` (equipped bag
   gems?) and `sockets`/`options`/`polish_step` persistence model — current
   `CharacterItem` lacks these fields (builder phase decision).
