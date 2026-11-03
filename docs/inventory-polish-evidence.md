# Inventory polish (item tempering) evidence

"Tempering" is the in-game name (blacksmith dialogue "I want to temper my gear."); the catalog,
packets and code call it polish.

Sanitized: no item-instance ids, unit ids, sequence numbers, timestamps or balances. Item
ids are static catalog keys. Labels: **Observed** (decoded capture frames), **Static**
(catalog or client code), **Candidate** (inferred).

## 1. Packets (Observed, 7 exchanges in the three new capture sets)

| Direction | Opcode | Packet | Fields |
| --- | --- | --- | --- |
| C->S | 20075 | `PC2S_ItemPolishReq` | `item_id` only (`add_material_item_cms_id` never sent) |
| S->C | 169 | `PS2C_ItemsUpdateNoti` | `items[{item_cms_id, grade, durability, polish_step, equip_slot?}]`, `money_changes[{change, result}]`, `stackables[{item_cms_id, quantity}]` (remaining count, quantity absent when 0), reason marker `polish: {}` |

No refusal and no error reply occurs; no `ChangeStatNoti` ordering is recorded here.
Polish is distinct from repair (`InteractUnitReq`, reason `shop`, see
docs/inventory-repair-evidence.md).

## 2. Rule source (Static, 7/7 exchanges reproduced)

`ItemPolish.jsonl` has 125 rows keyed by (`grade`, `equipCategory`, `polishStep`) with
`successProbability`, `failProbability`, `destructionProbability` (parts per 10000),
`materialId01`/`materialCnt01`, `costType` (Gold) and `costValue`. `polishStep` is the
TARGET step (the item's current step + 1). Categories are `Weapon` and `Armor` only,
grades Normal, Rare, Grand, Heroic, Legendary, steps 1 to 15. An item maps to its category
from its catalog `equipSlot`: `Weapon_*` to Weapon, `Armor*` to Armor; costume, bag,
accessory and tool slots have no rows.

| # | Item (grade, slot) | Step | Row cost | Row material | Observed change | Observed material left |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 13102102 (Grand, weapon) | 0 to 1 | 1000 | 90211101 x1 | -1000 | 1 |
| 2 | 11102302 (Normal, weapon) | 0 to 1 | 200 | 90211101 x1 | -200 | 0 |
| 3 | 13102102 (Grand, weapon) | 1 to 2 | 1425 (6000 success / 4000 fail) | 90211101 x2 | -1425 | 0 |
| 4 | 12102303 (Grand, weapon) | 0 to 1 | 1000 | 90211101 x1 | -1000 | 2 |
| 5-7 | 12401201, 12401101, 12401301 (Grand, armor) | 0 to 1 | 1000 | 90211201 x1 | -1000 each | 2, 1, 0 |

All seven succeeded; the new `polish_step` equals the previous step + 1.

## 3. What polish does NOT change (Observed)

Durability is not touched: the first weapon is 9908 before and 9908 after (the damaged value
is carried by the earlier 169 for the same item), and the other six stay at 10000. Polish is
not a repair. (An earlier version of docs/inventory-repair-evidence.md read a later,
unrelated durability frame as a before/after pair; that is corrected alongside this note.)

## 4. Client behavior (Static, IDA 2026-10-26)

Recorded in `dumps/ida_research/tempering-20261026/` (14 functions named and saved).

- **Request.** `AAC_ItemPolishWidget_SendItemPolishReq` sends `item_id` only; the shipping widget
  never fills `add_material_item_cms_id`. Before sending, the client only checks that the stone
  count of row `(grade, equipCategory, step + 1)` is owned; gold, equip state and durability are
  left to the server.
- **Cap.** `AAC_ItemPolish_GetMaxStepForItem` uses a map keyed by `(grade, equipCategory)` built
  from ItemPolish rows: 15 steps for weapons and 10 for armor in this catalog.
- **Result.** `AAC_ItemPolishResultPopup_SetResult` switches on `Reason_Polish.result`
  (`ItemRollResult`): Success and Fail look the item up in the inventory by `item_id`
  ("Tempering level remains the same." for Fail); Destruction looks the item up in the catalog by
  CMS id because the instance is already gone ("Equipment destroyed."). An `{item_id}`-only
  ItemsUpdateNoti entry removes a bag item (`AAC_Inventory_ApplyItemUpdate`).
- **Recovery.** No client code references the polish-recovery or great-success popup texts, and
  no request can ask for a recovery, so `Reason_PolishRecovery` is unreachable in this build.
- **Stats.** Every `AAC_Equipment_Calculate*` helper except fixed hit/impact, and the basic-stat
  bonus, multiply by `float(polish + 1)`, with 0 polish outside the item's increase table.

## 5. Server implementation

`ItemPolishCatalog` (`src/world/features/inventory/item_polish_catalog.cpp`) indexes the rows once at startup.
`InventoryRules::plan_polish` checks stone, additive and gold and returns the outcome chances;
`resolve_polish_roll` maps a server roll in `[0, 10000)` onto success, fail and destruction. The
world handler refuses before any roll or write, then commits the outcome with one
`Store::apply_item_polish` call: the item change, stone, additive and gold move together under
the character revision, in memory and in PostgreSQL. No schema change was needed.

| Outcome | Item | Charges | 169 entry | `polish` reason |
| --- | --- | --- | --- | --- |
| Success | step + 1 | stone, additive, gold | whole item | `{}` (as captured) |
| Fail | unchanged | stone, additive, gold | whole item | `{result: Fail}` |
| Destruction | deleted (must be unequipped) | stone, additive, gold | `{item_id}` only | `{result: Destruction}` |

An equipped success also sends 47 (every changed stat plus the vitals) and 168.

## 6. Candidate rules (no capture)

- Fail and Destruction charge the full row cost.
- Refusal codes: unknown item `ItemIdInvalid`; not temperable or no applicable row
  `ItemPolish_Invalid`; zero durability `ItemPolish_Damaged`; at the cap `ItemPolish_StepIsMax`;
  a non-additive `ItemPolish_InvalidAdditiveMaterial`; worn gear on a step that can destroy it
  `ItemPolish_RequireUnEquip`; short stone or additive `ItemCountError`; short gold
  `MoneyNotEnough`.
- Additives: one unit per attempt. `AddSuccessPercent` adds `value` percentage points of success,
  capped at certainty and taken from fail first, then destruction. `DestructionProhibition`
  turns destruction into fail.
- 47 after an equipped armor success.

## 7. Not established or not modelled

- The roll algorithm of the official server; no Fail, Destruction, refusal or additive frame.
- No ItemPolish row in this catalog has a destruction chance, so destruction and the Safeguard
  additive only matter for a future catalog. A destruction that would need to grant
  `polishResidueId` is refused, because no item sets it and the grant rule is unknown.
- Rows with a second material or a non-Gold cost (none in this catalog) stay refused.
