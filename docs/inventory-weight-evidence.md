# Inventory Weight Evidence

Evidence record compiled on 2026-10-25 by builder-5.

## 1. Bare Unequip (Observed)

From `out/captures/generic-part-2-20261024-162411/decoded-analysis.jsonl`:

- **Record 29369** (seq 2928, t ≈ 7059958 ms): Client sends `PC2S_ItemEquipReq` (20074) with only `slot: EX5EquipSlotType_FlightTool`. Field 2 (`item_id`) and field 15 (`packet_seq`) are absent. Protobuf payload: 2 bytes `08 17`.
- **Record 29371**: Server responds `PS2C_ItemsUpdateNoti` (169) with the vacated item (cms 11600103) carrying **no `equip_slot`** — item returns to inventory.
- **Record 29373**: Server broadcasts `PS2C_EquipmentsUpdateNoti` (168) with `item_cms_id` **absent** — visual slot 23 (`FlightTool`) is cleared.

IDA `sub_1462F4BD0` / `sub_14631FBB0` confirm: client sets `item_id = 0` (proto3 omits it). Client also blocks unequip of slot 24 (`Bag`) locally (`sub_14613A6B0` returns 9).

---

## 2. Weight Enforcement — Catalog Constants

From `data/generated/catalog/Tables/GameVariable.jsonl`:

| Variable | Value | Role |
|---|---|---|
| `BagDefaultSafeWeight` | 50000 | Baseline safe weight without an equipped bag |
| `BagMaxWeightMultiplier` | 200 | Hard ceiling multiplier (safe * 200?) |
| `StatEquipWeightValue` | 150 | Weight capacity per stat point (which stat unconfirmed; Candidate) |
| `StorageDefaultWeight` | 100000 | Non-character storage default |
| `InvWeightMoveSpeedStep2RatioFrom` | 0.5 | Move speed reduction threshold |
| `EquipWeightStepLightRatio` | 0.4 | Light encumbrance threshold |
| `EquipWeightValueLightRatio` | 0.25 | Light penalty ratio |
| `EquipWeightValueHeavyRatio` | 1.5 | Heavy penalty ratio |
| `EquipWeightValueVeryHeavyRatio` | 3 | Very heavy penalty ratio |
| `EquipWeightStepVeryHeavyRatio` | 9.99 | Very heavy threshold |
| `StatusEffectIdForInvWeightHeavy` | 50030004 | Heavy encumbrance status effect |
| `StatusEffectIdForInvWeightVeryHeavy` | 50030005 | Very heavy encumbrance status effect |

From `Item` catalog rows:
- `Item.weight`: per-unit weight (observed range 1–8800; potion 10, staff 1500, armor suit 4800, bag 100).
- `Item.safeWeight`: present on `Bag`-slot items (starter bag 11700201: 135000; range 200000–600000).

---

## 3. Weight Thresholds in Captures (Candidate)

No capture in the three new sets (`full-tutorial-capture-20261024-152100`, `generic-part-1-through-level-6-20261024-153513`, `generic-part-2-20261024-162411`) triggered a weight-limit refusal. Players remained within safe capacity throughout all sessions.

**Observed weight-related state changes in captures**:
- `PS2C_ChangeStatNoti` with `default_source: "SCS_Equip"` carries stat changes including stat_type 68 (`EX5StatType_ExhaustionGauge`, not weight) after equip/unequip operations, but no weight-limit rejection was observed.
- `PS2C_UpdateStatusEffectNoti` carries status effects 2130 (unknown) and 4000 (unknown) during combat, but no encumbrance status effects (50030004/50030005) were observed.
- `PS2C_StatSyncNoti` carries regen checkpoints but no weight-related stat sync failures.

**Conclusion**: Weight threshold crossings and their client/server behavior are **Candidate** — unobserved in captures. The encumbrance system (heavy/very heavy status effects) was not triggered in any captured session.

---

## 4. Error Codes (Static)

From `Catalog/Tables/ErrorCode.jsonl` and `Protocol/proto/ec.generated.proto`:

| Code | Name | SystemMessage Type | Role |
|---|---|---|---|
| 1004 | `ItemWeightLimit` | `SystemMessage_Text` | Hard weight capacity exceeded; user-facing error toast |
| 1005 | `ItemSlotLimit` | *(empty)* | Storage container slot capacity |
| 1024 | `ItemNotEnoughFreeWeight` | *(empty)* | Pre-check: free weight headroom < item delta |
| 6006 | `Npc_LootOverMaxWeight` | `SystemMessage_Text` | Loot refusal on `PS2C_LootItemRes` (26) |
| 900 | `WeightPenaltyState` | *(empty)* | Character encumbered state |

No capture shows any of these codes being returned. Whether 1004 vs 1024 is returned for equip vs grant vs loot paths is **Candidate**.

---

## 5. compute_weight Module Constants (Static)

The `InventoryRules::compute_weight` function uses these Static constants:
- `BagDefaultSafeWeight` = 50000 (baseline)
- Starter bag `safeWeight` = 135000 (from `Item.safeWeight` for cms 11700201)
- `StatEquipWeightValue` = 150 per stat point
- Per-unit weight: placeholder (10 per quantity) — real weight from `Item.weight` catalog field

**Note**: The module does NOT enforce 1004/1024 rejections. These remain Candidate until observed.
Carried weight now drives the movement-speed penalty described in section 7; refusals are still
not enforced.

---

## 6. Summary

| Item | Confidence | Evidence Basis |
|---|---|---|
| Bare unequip packet payload | **Observed** | Capture 29369 + IDA decompile |
| Server unequip notifications | **Observed** | Captures 29371, 29373 |
| Bag slot 24 unequip restriction | **Static** | IDA `sub_14613A6B0` |
| Error code numbers (1004, 1005, 1024, 6006) | **Static** | `ec.generated.proto`, `ErrorCode.jsonl` |
| 1004 vs 1024 distinction | **Static** | `ErrorCode.jsonl` type metadata |
| Weight threshold crossing behavior | **Candidate** | Unobserved in all captures |
| Encumbrance status effects (50030004/50030005) | **Candidate** | Unobserved in all captures |
| Weight stat type in ChangeStatNoti | **Static** | Stat type 68 = EX5StatType_ExhaustionGauge, not weight |
| Encumbrance statuses lower MoveSpeedRate (-500 / -1500) | **Static** | `StatusEffect` -> `MaintainEffect` -> `MaintainStat` rows |
| Client movement speed scales with stat 84 | **Static** | `AAC_Unit_Candidate_RefreshMoveSpeedFromStat` |
| Permanent status `expire_tick` = INT64_MAX | **Observed** | Captured 50030001/50030002 adds |
| Emulator tier thresholds (section 7) | **Candidate** | Server-side reading of catalog values |

---

## 7. Carried-Weight Encumbrance (implemented 2026-10-26)

### Static evidence

Catalog chain for both penalty statuses (`StatusEffect.FK_maintainEffects` ->
`MaintainEffect` type `Stat` -> `MaintainStat`):

| Status | ui_type | duration | MaintainStat |
|---|---|---|---|
| 50030004 (Heavy) | Debuff, icon 70009 | empty | `MoveSpeedRate` statAdd -500 |
| 50030005 (VeryHeavy) | Debuff, icon 70009 | empty | `MoveSpeedRate` statAdd -1500 |

`Stat.MoveSpeedRate` is limited to 200..1500 and the neutral player value is 1000, so Heavy halves
the speed (500) and VeryHeavy clamps to the minimum (200, one fifth of normal speed).

Client recovery (`dumps/ida_research/inventory-weight-20261026/`, saved in the `.i64`):

- `AAC_GameVariables_SetStatusEffectIdForInvWeightHeavy` / `...VeryHeavy` and
  `AAC_GameVariables_SetInvWeightMoveSpeedStep2RatioFrom` only parse the variables into the
  GameVariables object. No client consumer of the step ratio was found, so the tier rule is
  server logic.
- `AAC_Unit_Candidate_RefreshMoveSpeedFromStat` reads stat 84 (`MoveSpeedRate`), multiplies the
  base speed by `rate * 0.001` and hands the result to
  `AAC_X5Character_Candidate_SetMaxMoveSpeedForMode`, which writes the walk, crouch or swim
  maximum speed. The client therefore moves slower when its stat 84 is lowered. Whether a
  `ChangeStatNoti` for stat 84 triggers this refresh directly is not proven statically.

### Observed encoding used

- Server statuses without a catalog duration (50030001, 50030002) are sent with
  `expire_tick = 9223372036854775807` (INT64_MAX).
- Status-driven stat changes use `ChangeStatNoti.default_source = SCS_StatusEffect`, and the
  stat change precedes the add and the removal notification (status 100597, 3 of 3).

### Emulator rule (Candidate)

`src/world/features/inventory/inventory_encumbrance.cpp` owns the rule; `InventoryCapacity::measure` still owns the
carried weight (all owned items and stackables against the equipped bag `safeWeight`, otherwise
`BagDefaultSafeWeight`).

| Carried weight | Tier | MoveSpeedRate |
|---|---|---|
| up to the safe weight | none | 1000 |
| above safe, below safe + 50% of the overweight band | Heavy (50030004) | 500 |
| from safe + 50% of the band | VeryHeavy (50030005) | 200 |

The overweight band runs from the safe weight to the hard ceiling `safe *
BagMaxWeightMultiplier%` (200%, the same ceiling the NPC shop uses), and 50% is
`InvWeightMoveSpeedStep2RatioFrom`. With the starter bag (safe 135000) Heavy starts above
135000 and VeryHeavy at 202500; without a bag (safe 50000) at 50001 and 75000.

The world session re-measures after every packet batch that carries an `ItemsUpdateNoti` (169)
and when the character enters the world. A tier change sends `ChangeStatNoti` (stat 84 result),
then `RemoveStatusEffectNoti` for the old tier and `AddStatusEffectNoti` for the new tier
(level 1, stack 1, permanent expiry, instance ids from 0x40000000 so they never collide with
combat status instances). Equipment, trade and tempering stat refreshes keep the tier applied.

### Not established

- No capture crosses a weight threshold; the thresholds, the transition packet order between
  two tiers and the instance numbering are Candidate.
- `InvWeightSpRegenPenaltyRate` (stamina regeneration penalty), `WeightPenaltyState` (900) and
  the refusal codes 1004/1024/6006 are not applied.
- Original-client acceptance (visible debuff icon and slower movement) has not been observed yet.
