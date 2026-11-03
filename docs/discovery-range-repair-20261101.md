# Automatic discovery range repair (2026-11-01)

The user reported repeated **System: Target is too far.** notifications while
walking in the shared world, without targeting or interacting with anything.

## Observed cause

The server's own info log never names the refused request, so the original
client's `X5Packet.log` from the same live session was decoded offline with
`chronicles-replay` (4,400 frames, all decoded). Every
`PS2C_CommonErrorRes` carries the `packet_seq` of the request it answers.
Joining them by sequence gives 23 `TooFar` refusals, and all 23 answer an
automatic `PC2S_DiscoverAdventureObjectReq` (20050). No combat, loot,
gathering or quest request was refused with `TooFar` in that session; combat
logs each of its refusals separately and none was `TooFar`.

Joining the refused targets to the generated indexes:

| Owner | Refusals | Horizontal distance from reported position | 3D distance |
|---|---|---|---|
| Shared adventure objects (maps 9031, 9012) | 14 | 99.2–100.0 m | 99.5–105.8 m |
| Nuia Tears (map 9012) | 9 | 98.3–100.0 m | 100.4–112.2 m |

The largest horizontal distance is 99.97 m, while the height difference
reached 51 m. The client therefore triggers discovery inside a 100 m radius on
the ground plane. All three server discovery handlers (shared adventure and
tutorial objects through `TutorialSession`, Nuia Tears, Nui statues) instead
required the **3D** distance from the **server-tracked** position to be at most
100 m. Height on cliffs, and the few metres by which the tracked position lags
the client's, pushed those reports over the limit. Even reports with a 3D
distance below 100 m were refused because of the tracked-position lag.

This is observed original-client behavior from one live session, not a
recovered official server rule. Raw packets, IDs and positions stay private
under ignored `out/toofar-20261101/`.

## Changed contract

`quest_rules::within_discovery_range` is the single discovery-range check. It
measures the horizontal distance from the tracked position and allows
`kDiscoveryRangeMm` (100 m) plus the existing 5 m `kPositionToleranceMm`.
Height is ignored. All three discovery handlers use it.

Unchanged: discovery still grants no reward. Nuia Tears still require the
reported position within 5 m of the tracked position. Unknown targets, wrong
maps, dead characters and request-rate limits behave as before. Explicit
activation, collection and interaction keep their 3D ranges (10 m tears,
7 m statues and objects). A report more than 105 m away horizontally is still
refused with `TooFar`.

## Verification

New regression tests cover each handler: a target 103 m away horizontally and
45–51 m higher or lower is discovered, and one 106 m away is refused with
`TooFar`. The old 3D check refuses the first case, because 103 m exceeds
100 m before height is added.

- `chronicles-world-features-nuia-tests`: 45 passed.
- `chronicles-world-features-tutorial-tests`: 44 passed.
- `chronicles-world-features-adventure-tests`: 11 passed.
- `chronicles-world-features-quests-tests`: 53 passed.
- `chronicles-world-features-nuia-native-tests` (TLS discovery path): 3 passed.

The repair has not been deployed or accepted in the original client yet.
