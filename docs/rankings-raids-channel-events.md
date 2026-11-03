# Rankings, raids and channel events

This feature has behavior-producing runtime paths for the recovered 18-packet group. Its overall
status is **partial**, rather than complete official gameplay. Leaderboard queries and stored
operator-timed records work; raid registration/matching/lifecycle and map travel are served;
channel status and tribute offerings have a bounded local implementation. Authored event scripts,
raid encounters, automatic boss-score qualification and reward settlement remain unimplemented.

## Evidence boundaries

The recovered demo build is `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. The numeric mappings in
`protocol/opcodes.json` and fields in `protocol/descriptors.normalized.pb` are build-specific.

The official P2 capture contains 16 RankingListReq/RankingListRes exchanges: all returned empty
lists, reset counter 1, refresh timestamps about 10 seconds ahead and the next monthly reset at
2026-11-01 10:00 UTC. This establishes metadata and an empty-board response, not ordering of
nonempty boards or score submission. P2 also contains one ChangeChannelEventStatusNoti Success
in an interaction/cutscene sequence; it is not a tribute completion capture.

Static catalog inputs establish 16 Ranking rows, three RaidUI maps (10103, 20229, 10301), map
capacity 12, time limits, RankingUpdateCycleSec 10, list bounds 100 and RaidAutoEnterTime 10000 ms.
The recovered client function `AAC_UI_RaidMatching_ShowMatchedCountdown_Candidate` formats
RAID_MATCHING_Done, RAID_MATCHING_Time and RaidAutoEnterTime. Its caller and the UI event's
consumer are unresolved; the function does not itself send a raid move request. This is evidence
for a countdown UI, not proof of the official matching or entry policy. Its comment, versioned
function index and pseudocode are in `../../dumps/ida_research/rank-raid-20261026/`. IDA saved
`ArcheAgeChronicles.memdump.exe.i64` in place with stored-sections verification.

Synthetic C++/TLS tests, static recovery, official captures and original-client acceptance are
separate. No original-client trial of these additions or live deployment has been performed.

## Leaderboards

`src/world/features/events/ranking.*` loads Ranking and GameVariable through the verified ClientCatalog. Request 20006
returns response 9 with the ranking ID, record type, bounded results, own rank, next refresh,
reset counter and next reset. Unknown IDs are refused. Server and World record types currently
read the same local world board; no cross-server aggregation exists. Before storage access, each
connection is limited to 32 list queries per second, enough for the captured 16 rows and both
record-type tabs. Excess requests receive BadRequest with their packet sequence; this bound is a
local resource policy, not an official rule.

Monthly boundaries are at 10:00 UTC, matching the captured metadata. Daily 10:00 UTC and Monday
10:00 UTC weekly boundaries are explicit local choices: this catalog contains monthly rows only.
A holder keeps its best elapsed milliseconds; lower times rank first, then earlier creation time,
then stable holder key. A party composition has a sorted member-ID key. Display fields are copied
at record time. The own entry is reported even outside the top list. Previous periods remain
stored, and the reset counter advances from the first stored period (local policy).

MemoryStore and PostgreSQL implement the same best-entry write. PostgreSQL schema version 28
adds `emulator_ranking_records`; its conditional upsert compares and replaces atomically during
concurrent finishes. List entries, the owner's entry and its rank are read from one repeatable-read
snapshot; explicit C collation keeps holder-key ties consistent with MemoryStore in any database
locale. Records intentionally survive character deletion as historical results.

Operator commands start/finish/cancel one session-owned run. Times come from the server clock,
not client-supplied scores. A reset crossing refuses the finish. Every accepted finish consumes its
attempt, including a storage error: COMMIT may have succeeded before an error reached the session,
so a later finish must not silently retime that attempt. Operators must start a new attempt.
This is an operator test path:
there is **no automatic boss encounter qualification**, legitimate completion detection, ranking
reward settlement or reward mail. Party participants are currently chosen at finish from nearby
connected party members (100 metres, same map and instance), not from a recovered encounter roster.

## Raid channels

`src/world/features/events/raid_channel.*` owns a locked FIFO queue and matched-channel registry shared by world
connections. Registration, duplicate refusal, cancellation, matched rosters, leaving, disconnect
cleanup and timeout destruction produce packets 211–215. Cancellation/leaving validates the
connection holding the seat. Catalog limits and the entry countdown feed the timeout.

Local policy: a full group matches immediately; a partial group can match after 30 seconds with
at least one member. Only **one active group per raid map** is supported. Later groups wait so
different raid groups cannot be assigned simultaneously to the same map-only transport. Map reuse
must also wait for departing sessions to leave shared presence, including sessions still loading.
There is
no independent multi-instance raid simulation, role-based matching or recovered raid schedule.
Queues and channels are process-local and are dropped on restart.

A matched character may request its assigned map through 20091. The native session rejects an
invalid map, remote request coordinates, a non-parent source map, busy/dead players and private
tutorial/housing contexts. The destination preserves the server-tracked coordinates; an authored
raid-specific spawn/portal policy has not been recovered. The normal MapMoveStart/MapLoadComplete/
StartPlaying flow handles travel. Leaving or timeout returns the character to the entry point.
Persistence stores that parent-map point rather than an ephemeral raid position. Housing travel
captures the public return point before releasing the seat; a recovered housing origin that still
names a raid is redirected to that raid's public parent. Generic server/GM map travel cannot enter a
raid without the matching connection's valid seat and the entry handler's saved return point.
No raid bosses, scripted objectives, completion rewards or automatic encounter outcomes are
created by matching.

## Channel events and tribute

`tools/build_channel_event_data.py` exports verified map-to-zone-to-event bindings and channel-event
tribute altar placements. The current private export yields 111 map bindings and five map/altar
placements. Doodad 49054 offers item 100194 through interaction 401, whose UI is
OpenSharedOfferingForChannelEvent. Doodad-only offering UIs are excluded.

The listener loads `data/generated/channel-events/definitions.json` beside the configured catalog.
The normal export and persistent-start helpers regenerate it. Missing definitions refuse tribute
requests without charging items.

Request 20062 reports the nearest altar's count within 10 metres. Request 20081 validates its
server-map target, item ID, positive bounded quantity, owned stack, living state and range. The
board serializes inventory commits against other offerings and resets. A failed/throwing item
commit leaves the count and status unchanged; capacity failures do not call the item commit.
Only afterward are ItemsUpdateNoti.tribute and the resulting count published. Reaching target 10
sets Success. The range, target, threshold completion and nearby-observer audience are local choices,
not recovered official tribute rules.

Inventory costs are durable under the existing world-progress revision. **Event status and offering
counts are in memory**, so they are not crash-atomic with inventory and reset on restart. This is
not a durable public-event or reward implementation. There is no generic objective/task interpreter,
wave/spawn/timer script, qualification ledger, recovery, idempotent reward settlement, or integration
of CheckChannelEventStatus into every interaction/dialogue condition.

Request 20082 resolves an existing authored ungated channel boundary and requires the resolved map
to equal its requested map. It reuses `MapChannels` rather than trusting a client destination.
Gated destinations, unresolved spawn rules and generic scripted event entry remain unsupported.

## Game master commands

Use `/gm <command>` with a game-master account. The existing four-requests-per-second limit applies.

- `ranking_start <ranking_id>`, `ranking_finish <ranking_id>`, `ranking_cancel` exercise server-timed
  records and the response-7/8 paths.
- `raid_channel_enter <map_id>` forms a one-member raid match when that map is free. It does not
  spawn an encounter. The normal raid move request performs entry.
- `complete_channel_event_objective` sets the current map's event status to Success; it does not
  execute an unrecovered objective or grant its reward.
- `reset_channel_event_save_data` clears the map's in-memory status and tribute counts.
- `channel_event_info` stays owned by the existing diagnostics parser and reports the actual event
  binding and completion in DebugChannelEventInfoNoti (159), with no fabricated objective/save data.

Structured forms of the last four commands use the recovered GmCmd fields 119, 99, 100 and 98.
Text ranking commands are emulator-defined because no ranking GmCmd field was recovered.

## Validation commands

From `Emulator/`:

```powershell
python tools/build_channel_event_data.py
python tools/test_build_channel_event_data.py
cmake --build --preset release --target chronicles-event-tests chronicles-tests chronicles-server
ctest --preset release -R "Ranking|Raid|Tribute|ChannelEvent|EventProtocol|EventGm"
```

`RankingStorage.PostgresKeepsBestEntriesAndOrdersTheBoard` is gated by
`CHRONICLES_TUTORIAL_TEST_DATABASE_URL`; use a disposable PostgreSQL cluster only. The standard
`tools/test_postgres.py` runner includes this test when the event-test binary exists. The native
TLS case uses `CHRONICLES_CLIENT_CATALOG` and the generated channel-event data. Recovered packet
encoding, reset metadata, sorting/own rank, concurrent PostgreSQL upserts, FIFO/lifecycle/refusals,
connection guards, tribute commit failures and native dispatcher routing have synthetic tests.
See `VALIDATION.md` for the exact runs and outstanding failures/publication gap.
