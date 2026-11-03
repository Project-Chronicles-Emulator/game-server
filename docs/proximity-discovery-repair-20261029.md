# Automatic discovery and vendor target errors (2026-10-29)

The user reported repeated **Unable to find target** notifications while walking
or riding, without pressing interact. NPC interaction itself already worked.

## Observed cause

The current original-client packet log correlates 21 `Interaction_TargetNotFound`
responses (`PS2C_CommonErrorRes`, 2) to automatic
`PC2S_DiscoverAdventureObjectReq` (20050) requests by packet sequence. The verified
English localization maps error 4207 to the reported text.

Joining those targets to the verified current-map asset identifies 18
`LootingObject` reports and one each for `HersanderCairn`, `RecordStone`, and
`RavenMaumet`. None belongs to the Nuia Tear placement index. The shared-world
dispatcher previously claimed all discovery requests for that narrow index,
which then returned the target error. These are observed emulator failures in
the original client, not official-server success evidence.

Four separate `TooFar` discovery responses targeted actual indexed tears near
the configured discovery boundary. They are not the reported target-error code;
this repair preserves distance and reported-position validation.

Raw requests, IDs, positions and capture files remain private under ignored
`out/proximity-target-20261029/`.

## Changed contract

The Nuia Tear session now claims a well-formed discovery only when the tracked
map and target resolve to its placement index. Other discoveries continue
through the dispatcher without a tear error, progress load, revision write,
reward or fabricated success. They do not consume the tear request budget.

Malformed discovery retains the existing bounded `BadRequest` path. Explicit
activation still validates targets, life state, distance and position, and
commits collection and its reward atomically. Tutorial discovery and statue
ownership remain unchanged. Unknown adventure-object features remain partial;
silently declining another feature's report does not implement it.

## Verification

The subsequent Harbor City vendor report is a separate confirmed failure:
two shop opens and three stack sales returned `Interaction_TargetNotFound`
before inventory or gold changes. The authored general merchant uses a
21:00–04:00 client schedule. The static-NPC exporter excluded every selector
with a time window, removing an otherwise valid, visible interaction target.

The exporter now indexes scheduled client-owned actors as well as all-day
actors. All 726 previous targets remain byte-for-byte equivalent; the updated
profile has 1,428 map-scoped targets. Exact authored identity, map, template,
range, catalog conditions and atomic item/gold changes remain mandatory. No
duplicate NPC is spawned. Weather-dependent, disabled, adventure-controlled
and ambiguous placements remain excluded. The client owns scheduled actor
appearance; the server does not independently enforce opening hours. This is
an interaction-identity repair, not a server NPC scheduling implementation.

Before the repair, both the new session regression and the extended real-TLS
regression failed on the unwanted response. After the repair, the focused
46-test CTest selection passed with no skips or failures.

Coverage includes a burst of unrelated discoveries followed by a valid tear,
wrong-map ownership, malformed requests, no reward on discovery, collection
exactly once per character, and restored state after reconnect. Existing
activation refusal, range, rate-limit and atomic-commit cases also pass.

The full Release suite passed 1,868 tests with 123 optional skips and zero
failures (1,991 registered). After the vendor report, eight exporter tests
passed, including day/night schedules; the actual Harbor merchant TLS test
failed against the old profile and passed with the regenerated profile. It
verifies shop opening, five owned materials sold for the catalog refund of
five gold, persisted inventory/gold, and refusal of a second exhausted sale.
The complete merchant test executable then passed 53 cases with two optional
skips and zero failures against the updated profile.

## Live acceptance

Repair `220807a` was deployed through a graceful restart on 2026-10-29. The
running binary and all 545 manifest files verified, including the 1,428-target
static NPC index. Database schema 34, player-state counts, configuration and
HTTP routes were preserved; no player data was reset.

After reconnecting, the user explicitly confirmed that **both now work**:
proximity warnings stop, and selling to the Harbor City merchant removes the
item and increases gold. This is user-observed original-client acceptance of
these two reported cases, not acceptance of every interaction or a claim of
server-enforced NPC schedules. The bounded post-deployment capture recorded
the reconnect; buffered logs did not yet contain the final sale at collection
time, so its success is attributed to the user's confirmation.

The cloud protocol Page remains at its document-size limit; the last confirmed
publication hash must not be advanced without readback.
