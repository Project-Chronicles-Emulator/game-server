# Inventory merchant buy/sell evidence

Sanitized: no item-instance ids, unit ids, sequence numbers, timestamps or running
balances. Item and shop ids are static catalog keys. Sources: official capture sets
`generic-part-1-through-level-6` and `generic-part-2` (docs/capture-corpus.md), catalog
tables under `Catalog/Tables/` of the client export. Labels: **Observed** (decoded
frames), **Static** (catalog/schema), **Candidate** (inferred, not observed).

## 1. Packets (Observed)

Buy, sell, repair and shop open all use `PC2S_InteractUnitReq` (20068); there is no
dedicated buy/sell opcode.

| Step | Direction | Packet | Fields |
| --- | --- | --- | --- |
| Open shop | C->S | `InteractUnitReq` 20068 | `target_unit_id`, `param.interaction_id`, `param.shop = {}` |
| Open shop | S->C | `InteractUnitRes` 163 | `interaction_id`, `target_unit_id` |
| Buy | C->S | `InteractUnitReq` 20068 | `param.shop.buy_list[] = {item_cms_id, count}` |
| Buy reply | S->C | `ItemsUpdateNoti` 169, then `InteractUnitRes` 163 | reason marker `shop: {}`; see below |
| Sell | C->S | `InteractUnitReq` 20068 | `param.shop.sell_list[]`: `{stackable: {item_cms_id, count}}` or `{item_id}` |
| Sell reply | S->C | `ItemsUpdateNoti` 169, then `InteractUnitRes` 163 | reason marker `shop: {}` |

Dialogue and shop limits (open sequence):
- `DialogueStartRes` 161 is the reply to a **separate** client request,
  `PC2S_DialogueStartReq`, which the client sends right after the shop-open
  `InteractUnitReq`. It is not a reply to the 20068 request (Observed ordering: both
  requests, then 163 and 161).
- `ShopItemLimitNoti` 171 `{shop_id}` arrives roughly 400 ms after the shop-open request in
  the observed exchange, after 161. What triggers it, and whether an immediate 171 is
  acceptable to the client, is **Candidate**; only its payload (`shop_id`) is observed.

`ItemsUpdateNoti` precedes `InteractUnitRes` in all 7 buy/sell exchanges. `InteractUnitRes`
carries only `interaction_id`, `target_unit_id`, `packet_seq`. No error reply and no
refusal occurs in any of the 4 buys and 3 sales, so refusal codes are unobserved.

### Exchanges (all succeeded)

| # | Shop (interaction -> shopId) | Request | `money_changes[].change` | Item part of 169 |
| --- | --- | --- | --- | --- |
| B1 | 213 -> 1002 | buy 70109003 x1 | -10 | `stackables: [{item_cms_id: 70109003, quantity: 1}]` |
| B2 | 212 -> 1001 | buy 15010403 x1 | -200 | `stackables: [{item_cms_id: 15010403, quantity: 1}]` |
| B3 | 212 -> 1001 | buy 11200201 x1 (equipment) | -400 | `items: [{item_id, item_cms_id: 11200201, durability: 2000, grade: Normal}]`, no `equip_slot` |
| B4 | 212 -> 1001 | buy 133 x1 | -500 | `stackables: [{item_cms_id: 133, quantity: 1}]` |
| S1 | 116 -> 111103 | sell stackable 72000016 x4 | +4 | `stackables: [{item_cms_id: 72000016}]` (quantity absent) |
| S2 | 212 -> 1001 | sell stackable 70109003 x1 | +2 | `stackables: [{item_cms_id: 70109003}]` (quantity absent) |
| S3 | 212 -> 1001 | sell by `item_id` (cms 11200301, equipment) | +80 | `items: [{item_cms_id: 11200301, item_id}]` only |

`money_changes` entry: `{change, result}` as strings; `money_type` absent (Gold = 0).
`result` is the post-write balance and is intentionally not recorded here.

Shape notes:
- A sold-out stackable is reported with `quantity` absent (0 remaining), so the
  `stackables[].quantity` field is the resulting stack total (Observed for S1/S2; for
  buys the totals equal the bought count, so total-vs-delta is not separable: Candidate).
- A bought equipment item appears in `items` with identity, durability and grade and
  without `equip_slot`. A sold equipment item appears as `{item_cms_id, item_id}` without
  durability or grade, the same deletion marker form (Candidate: matches the sold entry
  only; no other deletion capture exists).

## 2. Price provenance (Static, 7/7 exchanges reproduced)

Item row fields: `priceType` (Gold), `priceValue`, `refundType` (Gold), `refundValue`.
Shop row (`Shop.jsonl`, keyed by `id` = shopId and `FK_itemId`): optional
`priceMultiplierPerMille`, `refundMultiplierPerMille`, `limitAmount`, `resetPeriodType`,
`fk_shopItemCondition`. `Interaction.jsonl` row `playerTaskCase: Shop`, `playerTask:
{"shopId": N}` maps interaction 212/213/116 to shops 1001/1002/111103.

| Item | priceValue | Shop multiplier | Buy charged | refundValue | Sell credited |
| --- | --- | --- | --- | --- | --- |
| 133 | 500 | none | 500 (B4) | 500 | n/a |
| 11200201 | 400 | none | 400 (B3) | 80 | n/a |
| 15010403 | 200 | none | 200 (B2) | 40 | n/a |
| 70109003 | 5 | 2000 per mille in 1002 | 10 (B1) | 2 | 2 (S2) |
| 11200301 | 400 | n/a | n/a | 80 | 80 (S3) |
| 72000016 | 10 | n/a | n/a | 1 | 4 for x4 (S1) |

Formulas matching every observed exchange:
- buy total = `priceValue * (priceMultiplierPerMille / 1000, default 1) * count`
- sell total = `refundValue * count` (no `refundMultiplierPerMille` was present in any
  sold-item shop row; its use is unobserved).

## 3. Server before the merchant change (9a2cd09f)

- `InteractUnitReq` (20068) is handled only by the tutorial session
  (`src/world/features/tutorial/tutorial.cpp:63` enum, `:1225` `case kInteractUnit`). `src/world/session/native_world.cpp`
  dispatches only `kItemEquipRequest` (`:1251`) and `kItemDiscardRequest` (`:1256`) on the
  non-tutorial path, so a shop open, buy, sell or repair request there receives no reply.
- `src/common/currency.hpp` provides `apply_money_change` (`:42`): underflow ->
  `MoneyNotEnough` 1201, overflow -> neutral internal failure.
- `Store::save_world_progress(before, after)` commits `gold` and `stackables` together
  under the revision, so a stackable buy or sell is one revisioned commit with existing
  APIs. Buying a new equipment row, or selling one together with a gold change, has no
  single existing Store transition (`grant_item` and `discard_items` do not carry gold).

## 4. Not established / not implemented

There is no working merchant. A shop-open reply exists but is reachable only in a server without
tutorial definitions, and stackable buy and sell are refused with BadRequest (fail closed, commits
0c35ed43 and 25a6ad37) until target NPC, map, position and range authorization exists; the
catalog price formulas behind the gate are unreachable and unexercised by tests.
These remain unimplemented or unobserved:
- Equipment buy (B3) and sell by `item_id` (S3): need a Store transition that carries a
  gold change together with an item-row grant or removal. Refused with the existing
  `BadRequest`, not emulated.
- Purchase limits (`limitAmount`, `resetPeriodType`) and the `ShopItemLimitNoti` contents
  beyond `shop_id`; shop conditions (`fk_shopItemCondition`).
- Refund rates beyond `Item.refundValue` (`refundMultiplierPerMille` is unobserved), and
  rounding of a fractional unit price (such a price is refused, not rounded).
- Inventory weight and slot limits on purchase (weight refusal semantics are unobserved).
- Any shop refusal code: none was captured. `MoneyNotEnough` 1201 is the currency module's
  proven underflow code, but it has not been seen as a shop refusal on the wire (Candidate
  for shops); other refusals use existing generic codes and are not observed behaviour.
- Whether a sold item must appear in the shop's `Shop.jsonl` rows: S1's item is in no
  shop row, so selling is not restricted to listed items (Observed for that item).
- The `DialogueStartReq` -> 161 handler for the non-tutorial path, and the trigger and
  timing of 171.
