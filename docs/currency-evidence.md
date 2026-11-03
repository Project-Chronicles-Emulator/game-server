# Currency evidence (Blue Salt and Hersander Token)

Bounded evidence pass, 2026-10-24, builder-6. Sources: three new captures
(`full-tutorial-capture-20261024-152100`, `generic-part-1-through-level-6-20261024-153513`,
`generic-part-2-20261024-162411`), `docs/inventory-equipment.md`, catalog `Money.jsonl`.

## Observed

`WorldCharMoneyNoti` (opcode 126):

- Bootstrap frame `update:false`, `money.values` empty observed in tutorial,
  then `update:true` with a separate `blue_salt` field.
- Open-world part 2 (level ~8) shows a snapshot containing both `values` keys
  `0` and `1`, followed by a separate `blue_salt` `update:true` frame.
  Exact balance values are intentionally not published.

- Bootstrap frame `update:false`, `money.values` empty observed in tutorial
  (`{"money":{}}`), then `update:true` with `blue_salt:"100"`.
- Open-world part 2 (level ~8) shows a full money snapshot:
  `{"money":{"values":{"0":"12259","1":"20"}}}` followed by
  `{"blue_salt":"100","update":true}`.
- Interpretation: `values` keys are `EX5MoneyType` ordinals (`0`=Gold,
  `1`=HersanderToken). `blue_salt` is a separate `int64` field and is **not**
  part of `values`. Mark: Observed.

Counts across three new captures: 4 `WorldCharMoneyNoti` frames total
(2 per tutorial/part-2, 0 in part-1).

`ItemsUpdateNoti` (opcode 169) `money_changes[]` (`MoneyChange{type,change,result}`):

- 157 entries total across the three captures.
- `money_type` omitted in 156 entries (default Gold); present as
  `EX5MoneyType_HersanderToken` in 1 entry (adventure-object activation).

  `EX5MoneyType_HersanderToken` in 1 entry (adventure-object activation,
  +20 → 20).
- No `blue_salt` delta was observed in any `money_changes` entry; blue_salt
  changes only via the separate `blue_salt` field on opcode 126.
- Reasons carrying money deltas, with counts:

| Reason | Entries | Money type |
| --- | ---: | --- |
| looting | 92 | Gold |
| system | 23 | Gold |
| shop | 12 | Gold |
| craft_action | 9 | Gold |
| adventure_object_activate | 9 | Gold |
| adventure_object_activate | 1 | HersanderToken |
| polish | 7 | Gold |
| death | 4 | Gold |

- Invariant holds in every frame: `result = previous_balance + change`.
- `money_type` absent ⇒ Gold (field 1 default), matching existing server
  behavior. Mark: Observed.

## Static (catalog)

`Money.jsonl`: `EX5MoneyType` Gold=0, HersanderToken=1. Both
`limitUserMax = limitSystemMax = 9 000 000 000 000 000` (9e15). Blue Salt is
not a `Money` row; it is the separate `blue_salt` int64 on opcode 126. Mark:
Static.

## Candidate

- Blue Salt write path (which server action mutates it) not observed. Paid-shop
  flow needed.

- Blue Salt write path (which server action mutates it) not observed; only the
  bootstrap `update:true` value of 100 appears. Paid-shop flow needed.
- HersanderToken earn/spend reasons beyond the single adventure-object frame
  unobserved; no shop spend of tokens captured.
- Whether `blue_salt` can go negative or has its own cap unknown.
- Whether `WorldCharMoneyNoti` always re-sends all of `values` or only changed
  keys unknown (part-2 snapshot shows both keys together).

## Persistence status on integration head a6dde5e6

Landed (Static, source and author-run tests; not independently reviewed):

- `emulator_characters.hersander_token` is persisted through schema version 10 and the
  revision-guarded `WorldProgress` save; Gold and Hersander Token share the single catalog limit
  (`kMoneyLimit`, 9e15) and out-of-range balances are rejected, never clamped.
- Gold rewards are credited through the currency module on a working copy that commits only if
  every reward succeeds; overflow past the limit is an internal failure reported with the existing
  BadRequest code, never `MoneyNotEnough` (1201).
- The entry money snapshot (opcode 126) carries the stored Hersander balance as key `1` when it is
  nonzero. Candidate: this rests on one captured snapshot and is not a proven wire rule; whether
  the server resends all `values` keys or only changed keys is unknown.

Not landed or unknown:

- Blue Salt has no persisted field, mutation source, cap or sign; no behavior is invented for it.
- Hersander Token earn and spend sources beyond the single adventure-object frame are unobserved,
  and no gameplay path grants or spends them.

## Persistence proposal (draft for builder; no migration applied)

Current: `emulator_characters.gold` column and `WorldProgress.gold`. Hersander
Token and Blue Salt have no persisted field.

Proposed schema change (draft; builder applies as a migration, not this agent):

```sql
-- draft, not applied
ALTER TABLE emulator_characters
  ADD COLUMN hersander_token BIGINT NOT NULL DEFAULT 0
    CHECK (hersander_token >= 0);
ALTER TABLE emulator_characters
  ADD COLUMN blue_salt BIGINT NOT NULL DEFAULT 0
    CHECK (blue_salt >= 0);

-- WorldProgress is in-memory/tutorial JSON; add matching fields there too:
-- WorldProgress.hersander_token, WorldProgress.blue_salt
```

Alternative (more general, matches `DbCharacterMoney.values` map): a
`emulator_character_money (character_id, money_type, amount)` table keyed by
`EX5MoneyType` ordinal, plus a separate `blue_salt` column (since blue_salt is
not in the money map on the wire). Builder to choose; the column approach is
simpler for the two known currencies.

Revision CAS: include hersander_token and blue_salt in the same
`save_world_progress` / character revision commit as gold and stackables, so
money mutations stay atomic. Bootstrap should send `update:false` with gold
only first, then `update:true` with blue_salt, matching observed ordering.

No source or migration edits made by this pass. Mark: Candidate until builder
integrates.
