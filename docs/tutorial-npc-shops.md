# NPC merchants: shop, repair, vouchers

Implemented 2026-10-26 for client build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`
(executable dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`). Owning sources:
`src/world/features/shop/npc_shop.*` (requests, access checks, planning, packets), `src/data/shop_catalog.*` (catalog
indexes), `src/data/shop_conditions.*` (condition evaluator), `merchant_interaction` in
`src/world/session/native_world.cpp` (session wiring) and `Store::commit_item_trade` in
`src/persistence/world_storage.cpp` (atomic commit).

Evidence labels used below: **Observed** (decoded official frames), **Static** (exported catalog
or recovered schema), **IDA** (client code), **Candidate** (inferred; no capture or client code).

## What each service does

| Service | Request | Evidence | Behavior |
| --- | --- | --- | --- |
| Open | `InteractUnitReq.param.shop {}` | Observed (5) | `InteractUnitRes`, then `ShopItemLimitNoti{shop_id, limits}` |
| Buy | `shop.buy_list[{item_cms_id, count}]` | Observed (4) | Gold or Hersander Token cost, new stack / housing count / item instance; `ItemsUpdateNoti` reason `shop`, then `InteractUnitRes` |
| Sell | `shop.sell_list[{stackable}, {item_id}, {trade_voucher_id}]` | Observed (stackable, item_id: 3); voucher Candidate | Credit from the catalog refund price |
| Repair | `param.repair.item_ids[]` | Observed (5) | Restores durability, charges gold; one `ItemsUpdateNoti` (reason `shop`, as captured) then `InteractUnitRes` |
| Voucher purchase | `param.voucher_group{good_cms_id, count}` | Schema + IDA; no capture (Candidate) | Issues trade vouchers (`ItemsUpdateNoti` reason `trade_voucher`) |
| Conditions | Interaction, shop good and voucher conditions | Static; evaluation rules Candidate | Evaluated against an empty context, see below |
| Buyback | none | Negative evidence | Not implemented: the client has no such flow in this build |
| Weight ceiling | purchases | Candidate | Rejects a purchase that would pass the ceiling |

Every captured money delta satisfied `result = previous balance + change` and omitted
`money_type` for Gold. Bought stacks appeared in `stackables` with the resulting total; a bought
tool appeared in `items` as a new instance with `item_id`, `item_cms_id`, `durability`
(= `Item.maxDurability`), `grade` and `created_epochtime`; a sold instance appeared with only
`item_id` and `item_cms_id`.

## Server authority (rule order)

`ShopCatalog` is built once at listener startup from `Item`, `Shop`, `Interaction`, `Npc`,
`GameVariable`, `Money`, `InteractionCondition`, `ShopItemCondition`, `ItemRepairCost`,
`VoucherGroup`, `VoucherCondition` and `TradeGood`. Malformed prices, currencies, multipliers,
limits, reset periods, grades or conflicting repair rows fail startup instead of being guessed. A
catalog missing any of these tables disables merchants and requests answer `Interaction_Invalid`.

A request passes these checks in order; each failure is a distinct `PS2C_CommonErrorRes` code
(**Candidate** as a rejection channel and code choice, but it is the channel the capture shows
for `TooFar` and the one every other world rejection uses):

1. Shape: decimal `target_unit_id` (non-zero, at most INT64_MAX, the registry's rule), positive
   `interaction_id`, at most 32 lines, one list kind, positive IDs and counts, no duplicate lines.
   `BadRequest`, `ItemCountError`, `ItemCmsIdInvalid`, `ItemIdInvalid`,
   `Interaction_InvalidParamType`.
2. Merchant: the target is a unit the server announced to this session in this map and instance
   (`Interaction_TargetNotFound`), its NPC template's interaction group contains the interaction
   (`Interaction_GroupIdNotMatch`), the request kind matches the interaction kind
   (`Interaction_InvalidParamType`), the interaction's condition holds
   (`Interaction_ConditionsNotMatch` / `Interaction_InvalidData`), and the server-tracked
   position is within 7 m in three dimensions (`TooFar`). A dead character cannot trade
   (`Interaction_Invalid`).
3. Rate: eight authorized requests per session per second (`BadRequest`). Rejected-at-step-2
   requests do not consume the budget because they never reach storage.
4. Purchase: item listed in that shop (`ItemCmsIdInvalid`), good condition holds
   (`Interaction_ConditionsNotMatch`), count at most `ItemMaxStack` (`ItemCountError`), a price
   and an emulator storage kind exist (`ItemError`), the unit price is integral
   (`Interaction_InvalidData`), the limit window has room (`PurchaseLimitExceeded`), stack and
   housing counts fit `int32` and 1,000 / 4,096 kinds (`ItemCountError`, `ItemSlotLimit`), at
   most 32 new instances each with a free slot in 0-999 (`ItemCountError`, `ItemSlotLimit`),
   weight (`ItemWeightLimit`), and each currency balance covers the total (`MoneyNotEnough`).
5. Sale: instance owned (`ItemIdInvalid`) and unequipped (`ItemEquipped`); stack or housing
   count owned (`ItemCountError`), quick-slot allocations stay backed (`ItemLocked`), installed
   housing objects are not sold (`ItemCountError`); voucher owned, unredeemed (`ItemIdInvalid`)
   and unexpired (`ItemHasExpired`); a positive refund exists (`ItemNonSellable`); the credit
   stays within `Money.limitUserMax` (`ItemError`).
6. Repair: item owned (`ItemIdInvalid`), flagged `npcRepairable` with an `ItemRepairCost` row
   (`ItemNotNpcRepairable`), at least one item actually worn (`ItemNotRepairable`), affordable
   (`MoneyNotEnough`).

All arithmetic is checked for signed 64-bit overflow. A plan copies `WorldProgress`, advances its
revision once and lists new, removed and repaired item rows. `Store::commit_item_trade` writes
Gold, Hersander Token, stack, housing and voucher rows, purchase-limit counters, item deletions
(unequipped only), durability restores (guarded by the expected old value) and item insertions in
one transaction under the previous `gameplay_revision`. A stale revision, stale wear, a foreign or
equipped item, or a slot collision rolls everything back, so a retried request cannot pay twice.
Tutorial sessions adopt the committed revision so their later commits still compare against
storage. An opened shop is not required before trading: the captured client bought without a
preceding open in one exchange.

## Prices, sales and limits

- Purchase cost = `Item.priceValue` x `Shop.priceMultiplierPerMille` (default 1000) / 1000 x
  count in the item's `priceType` (Gold or HersanderToken). **Observed** for Gold. A fractional
  unit price is rejected rather than rounded (unobserved). 75 of 731 shop goods are priced in
  tokens (**Static**); no token spend was captured, so the wire shape (`money_type` on the
  `MoneyChange`, which one adventure-object frame shows) is **Candidate** for shops.
- Sale credit = `Item.refundValue` x count, scaled by the selling shop's
  `refundMultiplierPerMille` when that shop also sells the item. Base formula **Observed**;
  the multiplier **Candidate**. Any merchant buys any item with a positive refund (the captured
  sale was of an item the shop does not list). Every refund in the catalog is Gold or empty.
- Limits: `ShopItemLimit.count` is the purchased count in the current window (an all-limited shop
  with no purchases listed nothing). Empty and `Once` periods never reset; daily and weekly
  windows start at `GameVariable.ResetBaseDateTime` (2023-01-02 05:00, a Monday), monthly windows
  on the first of the month at 05:00; UTC is assumed. **Candidate**.
- Repair cost per item = `ceil(ItemRepairCost.costValue x missing / Item.maxDurability)` with
  `missing = maxDurability - durability`. Fitted to two captured totals: one Grand longbow at
  durability 9908 cost 5, and a nine-item repair-all cost 207 (per-item ceilings sum to 207,
  per-item floors give 198, rounding the total gives 204). The same proportion is used for tools
  (max 2000); that is **Candidate**. `ItemRepairCost` joins `Item` by
  `category|equipmentType|grade|equipSlot` and 663 items are NPC-repairable. The client has an
  `ItemRepairCostDeclineRate` unit trait (string anchor only, **IDA**) that would discount
  repairs; it is not modelled.
- Items already at full durability in a repair request are skipped, because the client lists all
  equipped items in a repair-all request (**Candidate**).

## Conditions

`ConditionSet` evaluates rows of one `conditionId`: rows are grouped by `group`, a group combines
its rows with their shared `logicalOperator` after `negated`, and every group must hold
(cross-group AND is **Candidate**; merchants only use single-group conditions). Supported kinds:

| Kind | Used by | Evaluation |
| --- | --- | --- |
| `PioneerPermit {hasPermit}` | `Interaction` condition 704 on seven shop interactions (`hasPermit` false OR true) | Compared with the character's permit state; holds for everyone |
| `Company {companyId, level}` | `ShopItemCondition` 13/14/15 (BlueSalt 3/4/5, all other levels unused); `VoucherCondition` 1003/1004 | Company level >= `level` (a minimum is **Candidate**; exact tiers would give the same result today) |
| `CompanyLevelGreaterEqual {level}` | `VoucherCondition` 100-600 | The trade good's company level >= `level` |

Shop sessions fill the context from `CompanyService`: the character's current company and
position level, and whether its pioneer permit is valid. Company rows follow the client's check
order (same company, valid permit unless `ignorePermit`, minimum level; omitted fields use their
protobuf defaults and `EX5CompanyType_None` means no company). Condition 704 always passes;
company-gated goods (35 goods) answer `Interaction_ConditionsNotMatch` until the character holds
the required standing. A group that mixes `AND` and `OR`, a malformed parameter or an
unknown `typeCase` evaluates as unsupported and fails closed with `Interaction_InvalidData`; no
merchant condition in the catalog uses such a kind. Seven shop interactions carry condition 704 (132, 141-143, 223-225). Interactions 223-225 belong
to NPC templates 1001639-1001641, so the evaluated tautology is on a live path; 132 and 141-143
belong to no NPC template.

## Goods the emulator can store

| Item category | Stored as | Goods in shops | Evidence |
| --- | --- | ---: | --- |
| Consumable, CraftMaterial, PolishingMaterial, Etc, WayPoint, Memo, TearsOfNui | stack (`ItemStackable`) | 149 | Observed in `stackables` |
| Food, Repairer | stack | included above | Candidate (same kind, not observed) |
| Equipment | item instance (`Item`) | 114 | Observed |
| HousingObject, HousingBuilding | housing count (`HousingItemCount`) | 430 | Schema only; **Candidate** wire (HousingBuilding stored this way by the housing service, no shop sells one) |
| HousingPermit | stack | 1 | **Candidate** (the housing rent item, `docs/housing.md`) |
| HousingBuildingDrawing, CraftRecipe, QuestItem | none | 37 | Unsupported: `ItemError` |

Housing objects reuse `WorldProgress::housing_items` (`quantity`, `installed_count`), the same
rows the housing service moves between inventory and placed objects. The client's
`ItemsUpdateNoti.housing_items` update carries resulting totals by analogy with stackables.

## Trade vouchers (Candidate economy, IDA-grounded list handling)

`VoucherGroup` interactions (5 interaction rows; NPC templates 1001425, 1001673 and 1001960 use
group 32) issue vouchers. **No voucher exchange was captured.** Two rule sets exist:

- Trade-run rules (the normal server configuration, owned by the trade feature in
  `src/data/trade_catalog.*`, wired through `MerchantTradeInput::trade_catalog`): a voucher costs its
  `MakeTradeGood` production fee and materials (the client shows "Production Fee" and "Crafting
  Material"), lasts `TradeGood.validPeriod` minutes and at most `TradeVoucherOwnMaxLimit` live
  vouchers are held. Documented in `docs/vehicles-and-trade.md` with its own evidence labels.
- Fallback without trade rules (used by unit tests and catalogs lacking the trade tables):
  price = `TradeGood.basicValue` Gold per voucher, expiry = issue time + `validPeriod` hours,
  at most 10 vouchers per request. **Candidate.**

A sold voucher refunds `basicValue` x `TradeRefundBasisPoints` / 10000 (**Candidate**; the only
refund rate the catalog defines for vouchers). The client side is established by IDA
(`AAC_Trade_FX5TradeManager_ApplyVoucherDelta`, high confidence, recorded by the trade
research): in `ItemsUpdateNoti`, a `TradeVoucher` entry with `good_cms_id != 0` is appended to
the client's list and one with `good_cms_id == 0` removes the voucher with that `voucher_id`, for
reasons `discard`, `trade_loading` and `trade_voucher` (`WorldCharTradeVoucherNoti` replaces the
list). The server therefore issues vouchers with reason `trade_voucher` and removes a sold
voucher with a following `discard` update that carries only the voucher id. Voucher ids pack the
character id above a 20-bit per-character sequence.

## Weight ceiling (Candidate)

A purchase is rejected with `ItemWeightLimit` (1004) when carried weight plus the purchase's
weight would exceed `safe weight x GameVariable.BagMaxWeightMultiplier / 100`. The error codes
and variable exist (**Static**; 1004 carries a client message, 1024 `ItemNotEnoughFreeWeight` does
not) but no refusal was captured and the client's own use of the multiplier was not found
(**IDA**: the parse site is `AAC_Catalog_GameVariables_ParseCandidate`; the consumer is
unresolved), so both the percentage reading of `200` and the code are inferences. The
encumbrance feature (`src/world/features/inventory/inventory_encumbrance.*`, `docs/inventory-weight-evidence.md`) adopts the
same percentage reading for its overweight band. Carrying
more than the safe weight is not refused (encumbrance is a status effect, **Static**).

## Buyback

Not implemented. The client image contains no `Buyback`, `Repurchase` or `SoldItem` string, the
recovered descriptors have no sold-item list message, and no GameVariable describes one
(`dumps/ida_research/shop-20261026/`). Inventing a server-only list the client cannot display
would be unobservable behavior.

## Storage

- Schema v12 `emulator_character_shop_purchases (character_id, shop_id, item_cms_id,
  purchased_count, reset_epoch_ticks)`.
- Schema v15 adds `emulator_character_items.created_epoch_ticks` (zero for rows that predate
  tracking; the wire field is then omitted), `emulator_character_housing_items (character_id,
  item_cms_id, quantity, installed_count)` and `emulator_character_trade_vouchers (character_id,
  voucher_id, good_cms_id, created_epoch_ticks, expiration_epoch_ticks, redeemed)`.
- All of these are part of `WorldProgress` (loaded, compared and committed with the other
  revisioned rows). Like other progress rows they are updated in place and never deleted; a
  redeemed voucher stays as a tombstone so ids are never reused.
- Entry snapshots: persisted housing objects (`WorldCharHousingItemsNoti`, 131) and unredeemed
  vouchers (`WorldCharTradeVoucherNoti`, 142) are sent after the money snapshot when the
  character owns any (**Candidate** order; official empty snapshots were observed).

## Runtime boundary

The merchants are reachable only when the server announced them to the session. The tutorial
profile places no shop NPC (map 8000 and 10117 NPCs have no shop interaction group: the map-10117
Vadim spawn is NPC 1001539 with group 0). The map-9012 population file holds 23 shop, 10 repair
and 3 voucher placements and is announced when a character's persisted position is on that map;
nothing creates such a position and the original client has not entered it. **No original client
has traded with this implementation.**

Not implemented: ChangeStatNoti and EquipmentsUpdateNoti after a repair (the captured reply sent
both before `InteractUnitRes`; the emulator's stats do not yet depend on wear), the repair
discount trait, company and pioneer-permit systems, unsupported item categories above, mixed
buy/sell requests (rejected `Interaction_InvalidParamType`), and bought-item modifiers other than
grade, durability and creation time (options, sockets, bind state).

## Verification

From the checkout (the shared build tree can be unusable while other chats edit it; this change
was built from a private snapshot, see `VALIDATION.md`):

```powershell
cmake --build --preset release --target chronicles-tests
$env:CHRONICLES_CLIENT_CATALOG = (Resolve-Path data/generated/catalog).Path
ctest --preset release -R 'NpcShop.*|ShopConditions.*|NativeWorld.*Shop.*|NativeWorld.TutorialMerchant.*'
Remove-Item Env:CHRONICLES_CLIENT_CATALOG
```

`ShopConditions.*` and `NpcShop.*` cover the catalog joins, malformed rows, reset boundaries,
request parsing and every rejection above, token/housing/voucher/repair planning, the captured
prices and repair totals, response order and schema encoding. `NpcShopStorage.*` and
`Storage.PostgresShopTradeCommitsOrRollsBack` cover atomic commit and rollback (the latter needs
the isolated PostgreSQL cluster and applies schema v15 twice). The TLS test
`NativeWorld.TutorialMerchantServicesPersistAtomicallyAndKeepSessionRevision` drives open, buy,
sell, token, housing, repair and voucher requests through the world listener against announced
tutorial NPCs. These are synthetic checks, not an original-client trial.
