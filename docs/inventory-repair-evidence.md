# Inventory Repair Evidence

Evidence record compiled on 2026-10-25 by builder-5.

## 1. Repair (Observed)

Repair is performed via `PC2S_InteractUnitReq` (opcode 20068) with `param.interaction_id` and `param.repair.item_ids[]`. The target is an NPC unit (`target_unit_id`). The server responds with `PS2C_ItemsUpdateNoti` (169) carrying repaired items with full durability restored, `money_changes` with gold deduction, and `shop: {}` reason.

### Request: `PC2S_InteractUnitReq` (opcode 20068)

| Field | Type | Notes |
|---|---|---|
| `target_unit_id` | uint64 | NPC unit providing repair service |
| `param.interaction_id` | int32 | Interaction ID (65 observed) |
| `param.repair.item_ids` | repeated sfixed64 | Item IDs to repair |

### Response: `PS2C_ItemsUpdateNoti` (opcode 169)

| Field | Notes |
|---|---|
| `items[]` | Repaired items with full durability (10000) restored |
| `money_changes[]` | Gold deduction (`change` negative) |
| `shop: {}` | Reason marker (empty object) |

### Observed Repair Exchanges

**Exchange 1** :
- Request: `item_ids: [ITEM_A]` (weapon, cms 11102302, Weapon_1 slot)
- Response: durability restored to 10000, `money_changes: [{change: -4}]`

Six further polish exchanges are tabulated in docs/inventory-polish-evidence.md.
- Request: `item_ids: [ITEM_B, ITEM_C, ITEM_D]` (3 items)
- Response: all 3 items durability restored to 10000, `money_changes: [{change: -3}]`

**Exchange 3** :
- Request: `item_ids: [ITEM_E]` (weapon, cms 13102102, Weapon_2 slot)
- Response: durability restored to 10000, `money_changes: [{change: -10}]`

### Key Observations

1. **Full durability restoration**: All repaired items return to durability 10000.
2. **Gold deduction**: `money_changes` carries negative gold change. Amount varies by item (observed -3, -4, -10).
3. **Multiple items per request**: Up to 3 items repaired in a single request.
4. **No stackable consumed**: Repair does not consume stackable materials (unlike polish).
5. **No polish_step change**: Repair does not increment `polish_step`.
6. **Reason marker**: `shop: {}` on `ItemsUpdateNoti` (empty object), not `repair: {}`.

---

## 2. Polish (Observed)

Polish is performed via `PC2S_ItemPolishReq` (opcode 20075) with `item_id`. The server responds with `PS2C_ItemsUpdateNoti` (169) carrying the item with incremented `polish_step`, unchanged durability, `money_changes` with gold deduction, and a consumed stackable material. The `polish: {}` reason marker is present.

## 1. Repair Packet Pair (Observed)

From `out/captures/generic-part-1-through-level-6-20261024-153513/decoded-analysis.jsonl`:

### Request: `PC2S_ItemPolishReq` (opcode 20075)

Schema: `sfixed64 item_id; int32 add_material_item_cms_id`

| Field | Type | Notes |
|---|---|---|
| `item_id` | sfixed64 | Item to polish |
| `add_material_item_cms_id` | int32 | Material CMS id (default 0 observed) |

### Response: `PS2C_ItemsUpdateNoti` (opcode 169)

| Field | Notes |
|---|---|
| `items[]` | Polished item with incremented `polish_step` and unchanged durability |
| `money_changes[]` | Gold deduction |
| `stackables[]` | Consumed material (cms 90211101, qty 1) |
| `polish: {}` | Reason marker |

### Observed Polish Exchanges

**Exchange 1** :
- Request: `item_id` = weapon (cms 13102102, Weapon_2 slot)
- Response: durability unchanged (9908 before and after), `polish_step: 1`, `money_changes: [{change: -1000}]`, `stackables: [{cms: 90211101, qty: 1}]`

Six further polish exchanges are tabulated in docs/inventory-polish-evidence.md.

### Key Observations

1. **Polish increments polish_step**: From 0 to 1 (observed).
2. **Durability unchanged**: polish does not repair; the item keeps its durability (9908 before and after in the first exchange).
3. **Gold deduction**: catalog-driven per grade, category and step (200 to 1425 observed); see docs/inventory-polish-evidence.md.
4. **Material consumed**: Stackable cms 90211101, quantity 1.
5. **Reason marker**: `polish: {}` on `ItemsUpdateNoti`.

---

## 3. Comparison: Repair vs Polish

| Aspect | Repair | Polish |
|---|---|---|
| Request packet | `InteractUnitReq` (20068) with `repair.item_ids[]` | `ItemPolishReq` (20075) with `item_id` |
| Target | NPC unit (repair service) | Item directly |
| Durability effect | Full restoration (to 10000) | Unchanged |
| Gold cost | Small (-3, -4, -10 observed) | Larger (-200 to -1425 observed) |
| Material consumed | None | Stackable (cms 90211101, qty 1) |
| Polish step | Unchanged | Incremented |
| Reason marker | `shop: {}` | `polish: {}` |
| Multiple items | Yes (up to 3 per request) | No (single item) |

---

## 4. Catalog Constants (Static)

From `Catalog/Tables/Item.jsonl`:
- `durability`: base durability per item (observed 2000–10000)
- `polishStep`: present on items, incremented by polish

From `Catalog/Tables/GameVariable.jsonl`:
- No explicit repair cost or polish cost variable found
- `ItemMaxStack = 1000`

---

## 5. Rejection (Candidate)

No repair or polish rejection was observed in any capture. Whether insufficient gold or missing materials trigger error codes is **Candidate** — unobserved.

---

## 6. Summary

| Item | Confidence | Evidence Basis |
|---|---|---|
| Repair packet pair (InteractUnitReq → ItemsUpdateNoti) | **Observed** | 3 exchanges in capture |
| Repair full durability restoration | **Observed** | All items → 10000 |
| Repair gold deduction | **Observed** | change: -3, -4, -10 |
| Repair multi-item support | **Observed** | Up to 3 items per request |
| Repair no material consumed | **Observed** | No stackables in response |
| Polish packet pair (ItemPolishReq → ItemsUpdateNoti) | **Observed** | 2 exchanges in capture |
| Polish keeps durability, increments polish_step | **Observed** | 9908 before and after, step 0→1 |
| Polish gold deduction | **Observed** | change: -1000 |
| Polish material consumption | **Observed** | cms 90211101, qty 1 |
| Repair/polish cost formula | **Candidate** | No catalog constant found |
| Repair/polish rejection error codes | **Candidate** | Unobserved in captures |

| Field | Type | Value (sanitized) | Notes |
|---|---|---|---|
| `item_id` | sfixed64 | (item being repaired) | The equipped or inventory item to repair |
| `add_material_item_cms_id` | int32 | (material CMS id) | Catalog item used as repair material |

Observed frames:
- seq 4328: `item_id` = weapon (cms 13102102, Weapon_2 slot), `add_material_item_cms_id` absent (default 0)
- seq 4329: `item_id` = item (cms 10130402), `add_material_item_cms_id` absent (default 0)

### Response: `PS2C_ItemsUpdateNoti` (opcode 169)

Schema: `repeated Item; repeated ItemStackable; repeated MoneyChange; PacketSeq; reason fields`

Observed repair response (seq 4328):
```json
{
  "items": [
    {
      "created_epochtime": "...",
      "durability": 9908,
      "equip_slot": "EX5EquipSlotType_Weapon_2",
      "grade": "EX5ItemGrade_Grand",
      "item_cms_id": 13102102,
      "item_id": "...",
      "polish_step": 1
    }
  ],
  "money_changes": [
    {
      "change": "-1000",
      "result": "1411"
    }
  ],
  "packet_seq": {},
  "polish": {},
  "stackables": [
    {
      "item_cms_id": 90211101,
      "quantity": 1
    }
  ]
}
```

### Key Observations

1. **Durability**: Before repair = 8212 (from earlier frame at t≈3928499). After repair = 9908. Durability increased by 1696.
2. **Money change**: `change: -1000`, `result: 1411`. Gold deducted as repair cost. `money_type` absent (defaults to Gold=0).
3. **Stackable consumed**: `item_cms_id: 90211101, quantity: 1` — a repair material stackable was consumed.
4. **Polish step**: `polish_step: 1` on the item — repair increments the polish step counter.
5. **No separate repair packet**: Repair uses `ItemPolishReq` (20075), not a dedicated repair message. The `polish` field on `ItemsUpdateNoti` is empty (`{}`), confirming this is a polish/repair operation.
6. **Money reason**: The `ItemsUpdateNoti` reason field for this frame is `polish` (empty object), not `shop` or `system`.

### Second Repair (seq 4329)

- Request: `PC2S_ItemPolishReq` with `item_id` = item (cms 10130402)
- Response: `PS2C_ItemsUpdateNoti` with durability increase and money deduction (similar pattern)

---

## 2. Catalog Durability & Repair Cost (Static)

From `Catalog/Tables/Item.jsonl`:
- `durability`: base durability per item (observed 2000–10000 across items)
- `polishStep`: present on items, incremented by repair

From `Catalog/Tables/GameVariable.jsonl`:
- No explicit repair cost variable found. Repair cost appears to be derived from item grade/type.
- `ItemMaxStack = 1000` (GameVariable)

From `Catalog/Tables/Money.jsonl`:
- `Gold` and `HersanderToken` are the two money types
- `limitUserMax = limitSystemMax = 9e15` for both

---

## 3. Rejection (Candidate)

No repair rejection was observed in any capture. Whether insufficient gold or missing repair material triggers an error code is **Candidate** — unobserved.

Error codes in the 1000–1055 range that may apply:
- `ItemCmsIdInvalid` (1001)
- `ItemIdInvalid` (1002)
- `ItemBagInvalid` (1003)
- `ItemWeightLimit` (1004)
- `ItemSlotLimit` (1005)
- `ItemCountError` (1006)
- `ItemUnusable` (1007)
- `ItemHasExpired` (1008)
- `ItemEquipInvalidSlot` (1009)
- `ItemEquipInvalidItemType` (1010)
- `ItemEquipBroken` (1011)
- `ItemNonDiscardable` (1012)
- `ItemNonSellable` (1013)
- `ItemEquipped` (1014)
- `ItemNotEquipped` (1015)
- `ItemLocked` (1016)
- `ItemClassMismatch` (1019)
- `ItemEquipInvalidState_Combat/Dead/ClassActionForm` (1020-1022)
- `ItemNotEnoughFreeWeight` (1024)

Whether any of these apply to repair failure is **Candidate**.

---

## 4. Summary

| Item | Confidence | Evidence Basis |
|---|---|---|
| Repair packet pair (ItemPolishReq → ItemsUpdateNoti) | **Observed** | Captures (2 frames) |
| Money deduction on repair | **Observed** | `money_changes: [{change:-1000, result:1411}]` |
| Stackable material consumption | **Observed** | `stackables: [{cms:90211101, qty:1}]` |
| Durability increase | **Observed** | 8212 → 9908 |
| Polish step increment | **Observed** | `polish_step: 1` |
| Repair cost formula | **Candidate** | No catalog constant found; likely derived from item grade |
| Repair rejection error codes | **Candidate** | Unobserved in captures |
| Repair material item CMS | **Observed** | 90211101 |
