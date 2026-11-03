# Live issues of 2026-11-02: invisible walls, tutorial teleport, high RTT and GM teleport

Three reports from the hosted server were investigated from its logs (server build of commit
`27f651b` and its predecessor, both started 2026-11-02). A fourth report, the GM teleport from
the housing area followed by a refused re-entry, was investigated on the build of `380fbec`.

## Invisible wall at a travel volume

At 20:53:41 UTC a character cast a skill and, 6 ms later, its client sent
`PC2S_LevelTriggerReq` (20084) for a travel volume. The server logged
`Map travel deferred ... character has an active action` and sent nothing. The client keeps a
pending map transition after this request, which blocks movement at the volume until a server
map move arrives (see [zone-channel-travel.md](zone-channel-travel.md)). The player used two GM
position commands and two escape teleports three minutes later to get free. Earlier logs show
the same refusal on 2026-11-01 (four times) and 2026-11-02 (once), and four busy
`ZoneChannelMoveReq` refusals on 2026-10-30.

Cause: `travel_map_move` and `receive_zone_channel_move` refused busy, dead and GM-encounter
characters (the bounded travel keeps HP, statuses and cooldowns, so an action in progress must
end first), and nothing ever answered the waiting client.

Fix: the refused request is kept per session (`deferred_travel_` in
`src/world/session/native_world.cpp`; the latest request replaces an earlier one). Gameplay
ticks retry it every 250 ms; `next_gameplay_tick` includes the retry deadline, so the ordered
combat schedule and the plain timer both wake for it. The retry re-checks that the character
is still on the original map and no longer blocked, then performs the planned map travel, or
resolves the zone request again from the current position. A deferral expires after two minutes
and any map move that starts clears it. The unstuck command still reports its own refusal.

## Teleported back to the tutorial

At 20:51:32 UTC a character standing in the housing area (map 20101) sent
`PC2S_ZoneChannelMoveReq` (20090), which leaves the area, and was moved to map 8000 (the
tutorial start). The character had logged in inside the housing area after the 19:22 restart,
so no return origin was recorded; the documented fallback for that case was the tutorial start.

Fix: without an origin the character steps out onto the area's parent map where it stands.
`travel::Catalog::parent_arrival` applies the travel arrival rule to the parent map: the
housing area's authored area lies inside map 9012's, so the coordinates are kept. The tutorial
start remains the last resort when the profile names no parent arrival. A recorded origin is
still used first, as before.

## High round-trip time

The HUD showed an RTT of 559 ms; ICMP from the same desktop to the world host measured about
122 ms and a TCP connect about 140 ms. The client sends `PC2S_Ping` every 60 s (log timestamps
60.000-60.015 s apart), so the HUD value is one sample per minute. The server processed both
sessions with low load: the world process used about 2% of one core in a 10 s sample and the
host about 20% CPU, mostly Windows Defender (about 43% of one core). Packet log timestamps show
client frames processed in bursts right after skill requests, which can be client batching or
server queueing; the logs cannot separate the two. The ordered combat schedule deployed earlier
the same day routes every request, Ping included, through the population timeline.

No latency cause is established. The existing loopback-only `/internal/metrics` endpoint
reports frame strand latency, handler time and the client-reported round trip
(`PC2S_NetworkDelayReq`). It required the `--metrics` command-line switch, which the service's
backend config could not express (a config boolean expands to `--metrics true`, and the value
was rejected). `--metrics` now accepts that value, so `"metrics": true` in `backend.json`
enables the endpoint for the next measurement.

## Verification

- `MapTravel.ParentArrivalKeepsThePositionOnTheParentMapAndNeedsAParent` and the generated
  profile check that map 20101 exits onto 9012 in place.
- `HousingSessionTest.LeavingWithoutARecordedOriginStepsOutOntoTheParentMapInPlace`.
- Native TLS: `NativeWorld.TravelVolumeEnteredWhileDrinkingAPotionStartsAfterTheDrinkEnds` and
  `NativeWorld.RegionCrossedWhileDrinkingAPotionCompletesAfterTheDrinkEnds` send the travel
  request during a potion drink and expect `MapMoveStartNoti` only after the skill end. With
  the retry disabled, the volume test times out waiting for the map move.
- Existing zone, cave, region and gliding handoff TLS tests pass.

These are synthetic checks. No original-client observation of the deferred travel or of the
parent-map housing exit exists yet.

## Follow-up: loading screens at some borders

After the deployment the player reported loading screens instead of a continuous world in some
areas, for example while gliding. The hosted log showed zone requests on map 9012 answered with
the same-map recovery. The client was asking for zones nested in the field (villages, points of
interest, the housing area), which the server did not export; a same-map answer fails the
client's seamless check and loads. The repair exports nested zones, resolves the deepest one,
returns to the parent when leaving, uses the reported request point and checks map conditions.
Details and limits: [zone-channel-travel.md](zone-channel-travel.md) ("Seamless nested zones").
Native TLS `NativeWorld.VillageZoneIsEnteredAndLeftAtTheReportedPoint` enters village 9013 from
9012 and leaves it again with the real export; `MapChannels.Nested*` and
`HousingSessionTest.WalkingInEntersInPlaceAndWalkingOutReturnsToTheReportedSpot` cover the
precedence, gating flag and housing walk-in. Original-client acceptance is pending.

## GM teleport from the housing area and the refused re-entry (23:53 UTC)

A game master standing on a housing lot (map 20101) typed `/gm tp` with harbor coordinates of
map 9012. The command builds its destination on the current map, so the server moved the
character to the same coordinates on map 20101. The housing area has no NPC population and the
GM map move does not re-send the channel's lots, so the client showed an empty map. The player
then returned to character selection (`PC2S_CharacterExitReq` 20002 at 23:53:30.097), picked
the character again 2.7 s later, and the next world connection completed TLS at 23:53:33.519.
The server logged `Native world credential rejected` 1 ms later. Every later connection of that
game process failed during the TLS handshake with a fatal `handshake failure` alert sent by the
client, and the client reported "No response from the server". A fresh game start at 23:56
entered normally.

Causes:

- GM coordinates typed inside a housing area named map 20101 instead of the field it overlays.
  Housing areas share their parent map's coordinates (see [housing.md](housing.md)).
- The world entry arrived while the previous session was still closing. `consume_packet_key` and
  `consume_world_token` leave the ticket in place when the character is still admitted to its
  previous session, and the server closed the new connection at once. Server-side tests
  already retried in that case, but the stock client cannot reconnect after a refused entry; the
  same repeated TLS failures were recorded after the 2026-11-01 adventure-reward drop
  ([protocol-feature-reference.md](protocol-feature-reference.md), interaction section).
  The log has no direct record of the busy admission, because a busy token entry and an unknown
  token share the same warning. The ticket was created by the selection 0.7 s earlier and select
  never refuses, so the busy admission is the only cause consistent with the log.

Fixes:

- `travel::Catalog::housing_area_parent` names the field of a `HousingArea` map. `tp` and
  `up/down/fwd` typed inside map 20101 now build their destination on map 9012, which then picks
  the public region channel that holds it as before. `tp_map` and `goto` keep their own map.
- A busy world entry now waits for the previous session to release the character. The session
  keeps the credential and retries the same ticket every 100 ms on an I/O timer, logging
  `World entry connection=N waits for the character's previous session to close` and, once
  admitted, the approximate wait. `NativeWorldListener::set_entry_admission_wait` bounds the
  wait; the hosted server sets 10 s (`kWorldEntryAdmissionWait` in `src/apps/server.cpp`).
  Listeners created without that call, including the existing tests, still refuse at once.
  After the wait is exhausted the entry is refused and logged as before.

Limits: a waiting entry cannot see whether the client gave up, so a client that disconnects
during the wait may still be admitted and then closed normally. The stock client's own
response timeout is not known. The cause of the client's TLS failures after a refusal is not
established; this change avoids the refusal rather than repairing the client state.

Verification:

- `MapTravel.HousingAreaParentNamesOnlyTheFieldUnderAHousingArea` and the generated profile
  (20101 -> 9012; instances, dungeons and fields have no housing parent).
- `GmFeatureTest.CoordinateTeleportsFromAHousingAreaLandOnItsField`: `tp` and `up` typed on
  map 20101 move the GM to map 9012; a region that owns the destination still selects its
  channel.
- `Auth.NativeKeyAndDevelopmentTokenShareSingleUseHandoff`: a busy `consume_world_token`
  reports `admission_busy` and keeps the ticket.
- Native TLS `NativeWorld.ReconnectWaitsForTheClosingSessionWhenTheListenerAllowsAWait`: the
  closing session's position settlement is paused, the reconnect with the same ticket is not
  admitted while it is paused, and receives the key-exchange answer after the release, with the
  saved position intact.

These are synthetic checks. The original-client re-entry and the GM teleport out of the housing
area have not been observed on the corrected server yet.
