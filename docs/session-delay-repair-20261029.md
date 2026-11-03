# Mounted session delay investigation and repair

## Observed failure

On 2026-10-29, the original client became unable to dismount from a vehicle or return to
character selection. The deployed process and HTTPS health route remained available. The
client capture contained dismount and exit requests while the world service was still
processing movement requests hundreds of packets earlier. Packet processing intervals grew
from less than one second after boarding to approximately thirteen seconds. A bounded debug
window showed the next frame already read, with socket writes completing immediately, while
completion of the frame's worker processing was delayed. This establishes a session processing
backlog rather than a server process exit.

The affected connection was closed through the supported operator connection-kick endpoint,
which uses normal session teardown. No player data was reset. Raw captures and operational
records are retained under ignored `out/housing-completion-20261029/`.

## Committed rider life state

`CompanionSession::dismount_if_dead` previously loaded the entire persisted world-progress
record whenever the character was riding. Native session orchestration calls this both after
each request and on gameplay timer ticks. The production progress adapter opens a PostgreSQL
connection and reads the character's progress tables for each call. Consequently, boarding a
horse or vehicle introduced synchronous database work on movement, ping and timer processing.

The session now initializes its rider life flag from the existing entry snapshot and updates
it through `PlayerLifeCommitted`. Death and revival events are emitted only after a successful
persistence commit. Ordinary combat snapshots cannot change this flag, so failed death or
revival transactions do not cause a speculative dismount. Reentry reloads the entry snapshot.
The wire messages and dismount reasons are unchanged.

## One outstanding gameplay tick

The previous scheduler cleared its deadline as soon as a timer fired, before the worker
started. A slow request could therefore complete while that timer work was waiting, rearm
the same overdue deadline and enqueue another tick. Independent tick completions could keep
multiple timer chains alive. Canceling a wait also did not reject a successful callback that
had already entered the I/O queue.

An I/O-owned gate now keeps one tick in flight through worker execution and I/O completion.
A generation check rejects callbacks belonging to replaced waits. The worker computes the
next deadline from the current session state; map-loading and no-feature early returns also
release the gate. The change bounds pending timer work without dropping player requests.

## Bounded processing diagnostics

The existing administrative log-level control can enable debug timing of frame preparation,
handlers, observers and post-processing. Only steps exceeding 100 milliseconds are logged,
with opcode, static step name and elapsed time. Normal information-level logging does not
read the clock for these timers. Diagnostics contain no request payload or player identity.

## Verification boundary

Focused tests cover repeated mounted movement without additional progress reads, committed
death dismounting once, revival, reentry, diagnostic privacy and unchanged rejection behavior.
Deterministic scheduling tests cover repeated overdue frame deadlines while a tick is queued,
replacement of ready canceled waits, map-loading completion and unchanged deadlines.
The native companion tests exercise mounted movement, boarding and rider death over TLS.
The expensive read is a confirmed blocking operation; attributing the entire growing live
backlog and claiming recovery requires the post-deployment original-client driving trial.

The full Release suite completed: 1876 passed, 124 prerequisite-gated skips, zero failures
(2,000 registered cases). All three original-catalog TLS companion scenarios passed.
The reference generator tests passed (11 cases). The local generated reference and source
hashes agree. Its published Page remains stale because the existing document exceeds the
Page patch-size limit; the last confirmed publication record is deliberately preserved.

## Deployment and original-client acceptance

Repair `f44f51f` was deployed on 2026-10-29 after a verified database backup and graceful
shutdown. All 545 payload hashes matched, both HTTPS and world listeners were owned by the
new process, and the existing route statuses, schema 34 and player-state counts were preserved.
No database reset or migration was performed. An independent agent reviewed the staged
package hashes and deployment gates before the switch.

After reconnecting, the original client boarded a vehicle and drove normally. A bounded debug
window sampled 65 movement frames: 0-181 ms from completed frame read to worker completion,
with two 180 ms position checkpoints. Normal sampled movement completed in approximately
1-2 ms. These durations include worker scheduling, not network round-trip time. No worker
failure appeared. The player then confirmed the requested drive, dismount, NPC interaction and
character-selection trial worked. This establishes acceptance of the reported growing backlog
in that trial; it does not establish general server capacity or zero latency for every feature.
