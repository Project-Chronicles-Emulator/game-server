# Outstanding projectile immunity retention

The local candidate preserves a player's elapsed action-stage immunity for an already
outstanding projectile. When the action ends or changes stage, each flight retains only
the immunity applicable at its own impact deadline. Retiring the action therefore cannot
turn a previously immune impact into damage during a later retry.

The retained data belongs to the flight and is released with it; the existing 256-flight
limit bounds its lifetime and count. Immediate chained effects use the same retained
immunity. A newly spawned secondary flight has its own impact and does not inherit it.

Cancellation or stage retirement wins at the exact cutoff. Impacts strictly before the
cutoff can retain the old stage's immunity; impacts at or after it cannot. The cutoff is
also bounded by the authored stage duration. A later action cannot protect an earlier
impact, including an impact less than one millisecond before that action started.

Dead players cannot receive projectile hits, and revival discards outstanding old flights.
This repair does not preserve action history across death, revival or world departure.

## Verification

The Release combat test target built successfully. Its complete default suite ran 373
tests: 353 passed, 19 generated-profile-dependent tests skipped, and one failed.
The existing `WorldMobs.CandidateProjectileRetryPreservesImmunityAtOriginalImpactTick`
regression now passes. The new lifetime and stage-transition tests cover natural expiry,
cancellation before/at/after impact, a newer dodge, the natural stage-end boundary,
automatic stage changes and explicit stage changes. Returned packets pass protocol encoding.

An independent source review accepted this bounded change. This is synthetic/component
evidence; no updated native TLS trial, original-client acceptance or deployment is claimed.

## Remaining release blockers

`WorldMobs.EarlierStunFlightPrecedesAnNpcFlightOnLateReverseSessionPoll` still fails:
a later NPC flight can resolve before another session processes an earlier stun.
The population timeline and input-admission helpers remain disconnected from native gameplay.
Timer-driven combat quest events also lack a complete recoverable producer/settlement path.

Per-flight action history does not provide general transactional projectile retries.
It does not recover an earlier cancellation that itself has not executed, an overdue
flight created only after action retirement, historical shield activation, historical
positions or all other mutable combat state. Those limits remain release blockers where
the combined candidate depends on stronger chronology or recovery guarantees.

The complete combined release has not passed review. The local reference retains pending
source-review markers; no bulk source-hash refresh certifies the pre-existing candidate.
The reference generator's 11 tests pass. Strict checking still rejects pending source
reviews. The Page service rejected the guarded follow-up insertion with `page_limit_exceeded`
(`markdown_bytes`, maximum 1,048,576); the published mirror remains stale and its last
verified publication fingerprint is unchanged.
