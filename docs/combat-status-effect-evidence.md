# Combat status effect evidence (issue #63)

**Correction.** The version committed as `cdc0ba3` contained per-ID durations ("~30s", "~60s"),
"typical stack counts" and a claim that IDs `2001005` and `7000000` were possibly missing from the
catalog. Those values were not measured. This revision replaces them with numbers computed
directly from the captures and the catalog. Labels: **Observed** = decoded official capture,
**Static** = catalog (`data/generated/catalog/Tables/StatusEffect.jsonl`).

## Method (Observed)

Source packets in the three 2026-10-24 capture sets (`docs/capture-corpus.md`):

- `PS2C_AddStatusEffectNoti`: `status_effect.{status_effect_id, instance_id, level, stack_count,
  start_tick, expire_tick}`, `unit_id`, `caster_unit_id`. 1,318 notifications, 45 distinct ids.
- Duration = `(expire_tick - start_tick) / 1,000,000` (ticks are nanoseconds; same unit the
  skill-event normalisation already uses). `expire_tick = 9223372036854775807` (int64 max) is
  counted as "no expiry" and excluded from the duration column.
- Only CMS ids and aggregates are recorded; no unit, instance or session identifiers.

## Results

- All 45 observed ids exist in the 507 exported `StatusEffect` rows. No id is missing.
- `stack_count` is 1 and `level` is 1 in every one of the 1,318 notifications.
- Within one id the observed duration never varies (min = median = max) for all 43 ids that
  have an expiry.
- Observed duration equals the catalog `duration` (milliseconds) exactly for 40 of the 42 ids
  that have both an expiry and a non-empty catalog duration, including all of the most frequent ones (for example 2001005:
  1000 ms, 357 adds; 4000: 20000 ms, 61 adds; 2120 and 2125: 12000 ms; 7000000: 60000 ms).

### Differences

| id | adds | observed | catalog `duration` | note |
| --- | ---: | --- | --- | --- |
| 2001020 | 1 | 1400 ms | 1500 | 100 ms shorter |
| 1120101 | 1 | 3000 ms | 1000 | observed is 3x catalog |
| 50030003 | 2 | 300000 ms | empty | catalog has no duration |
| 50030001 | 201 | no expiry | empty | catalog has no duration |
| 50030002 | 12 | no expiry | empty | catalog has no duration |

Two single-sample differences (2001020, 1120101) may come from level or stat modifiers or from
a different source of the effect; the capture does not say. This is **Candidate**, not a
catalog error.

### Empty catalog durations

5 of 507 catalog rows have an empty `duration`. The generator writes `duration_ms = 0` for
them. Two of the five were observed without expiry and one with 300000 ms, so a zero or empty
duration does not by itself mean "permanent". The 9,000,000 ms value seen in row `1` is an
ordinary duration, not the no-expiry encoding (the earlier claim to the contrary was unverified).

## Update notifications

`PS2C_UpdateStatusEffectNoti` appears 1,612 times; the most frequent ids are 4000 (686),
2001005 (509) and 1120102 (173). The update semantics (refresh versus stack change) are not
analysed here and remain **Candidate**.

## Generator impact

The generator already exports all 507 rows. It now also checks that every `addStatusEffect`
or `removeStatusEffect` effect refers to an exported row (see
`docs/combat-effect-kind-contract.md`). It does not emit observed durations.
