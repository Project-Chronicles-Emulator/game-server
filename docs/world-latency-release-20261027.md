# Hosted world latency investigation — 2026-10-27

## Production observation

The existing hosted binary (SHA-256 `0123006C76D92106B9B91CA496C0F58C143F8A09F63E6140D4D5DE0B5CE79A7F`)
remained healthy on the HTTPS and world listeners after a routine restart. PostgreSQL and the
account/character admin queries remained healthy. With two players in the world, the process used
5.906 CPU seconds during one 5-second sample. The final 10,000 lines of one pre-restart log
covered 31.6 seconds and contained 4,967 outbound frames, including 4,906 `PS2C_MoveNoti`
frames. Each frame generated two synchronous `info` transport lines. A later two-player sample
contained 4,901 outbound frames in 43.7 seconds, of which 4,692 were movement frames.

Five ICMP probes from the local client computer to the hosted address were 144 ms with no loss.
The reported in-game RTT was around 426 ms. ICMP and game RTT have different paths through the
application, so this comparison only shows that the extra delay is plausible above the network
route. A separate live log review found 40–175 ms from frame read to processing completion for
client requests, including 114 ms for `PC2S_Ping`, while one player was online. The two-player
sample sometimes exceeded one second between a frame-length read and the post-processing log;
that interval also includes payload receive and must not be treated as a pure worker-queue metric.

A runtime `warning` log-level trial removed the per-frame `info` lines without changing the
binary. CPU did not fall in the short sample, but player activity was not held constant. The
trial does not establish the logging cost in isolation, and the original `info` level was
restored.

## Candidate scope

The isolated candidate is based on the known production source revision `4bf7955`. It caches
executable NPC attacks, range bands and the ranged classification once when `CombatData` loads.
The AI tick then reads the cached options instead of copying NPC skill JSON and traversing
effects and projectiles for every map mob. Skill order, cooldowns, range limits and attack
selection remain the same. World TCP sockets use `TCP_NODELAY`; per-frame transport traces move
from `info` to `debug`. The wire format and application requests are unchanged.

The source also synchronizes every authored civilian patrol position in the session registry
before processing every client packet. This extra work includes pings and movement packets. The
candidate skips that registry scan for ping, clock and movement requests. Sight updates calculate
patrol poses from the shared route directly, and targeted requests still synchronize immediately
before interaction and shop range checks. Periodic patrol updates continue to refresh the
session registry and movement notifications.

## Verification limits

The generated busiest map has 812 world-mob spawns. An isolated Release benchmark of the
unchanged code averaged 14.327 ms over 50 steady-state ticks and failed its 10 ms packet budget.
The candidate averaged 1.049 ms over the same 50 steady-state ticks, a 13.7-fold reduction.
The profile SHA-256 matches the live server's generated world-mob profile. The machine was
also running unrelated builds, so these are comparative results rather than stable absolute
server capacity measurements.

The full CMake Release build passed, including the server, launcher and auxiliary test
executables. The focused `WorldMobs`, `CombatData`, `NpcAi` and `NpcPatrol` run passed 39 cases
and skipped one optional profile fixture. Four additional native Ping, civilian patrol,
merchant and ownership checks passed with the documented process-local OpenSSL CPU dispatch
setting. Full CTest passed all 1,304 registered cases: zero failures and 101 environment-gated
skips. The first isolated run had one missing generated-catalog fixture path; linking this
worktree to the existing shared generated data made that case pass, and the complete rerun
passed.

The SHA-256-verified executable and manifest are staged on the hosted server, and all 543
existing runtime manifest files were verified before replacement. The database has a fresh
verified archive backup. Production still runs the previous binary. A later human release
instruction requires all active Codex and Cursor agents to finish before the combined live
update, so no server switch or Discord publication occurred in this validation pass. Original-
client RTT and a representative 500-player load test remain unmeasured.

## Live Project Chronicles 0.2.4 observation

On 2026-10-27 at approximately 12:48 UTC, the hosted service was independently checked
against source revision `4f7aa8a1e1237d10f96e519aab99e5274574995c` and server SHA-256
`a5ac48bed180664a9ce083b284e99676aaccf54fb8fea06459fd6b0126ea3cbb`. Both listeners and
the health route were ready. This release already contains the latency runtime changes
as commit `fc98ac6`; the older isolated artifact above is superseded and must not replace it.

The shipped 0.2.4 player launcher entered the original client, build `v26.10.18.734`, on
tutorial map 8000. Several observed HUD frames displayed **135 ms RTT**, including during
combat. Five ICMP probes from the same computer measured 145, 144, 144, 144 and 144 ms.
These initial measurements cover one player on the tutorial map and do not establish a
many-player capacity result.

A later observation on the same verified server binary showed **132 ms RTT** while the
client stood near a quest NPC on public map 9012. A simultaneous authenticated admin read
confirmed one player on that map. This is an original-client public-map sample after manual
gameplay; it is not a sustained movement/casting acceptance run or a many-player load result.

The guarded world TLS helper recorded restoration of both temporary changes. The separate
zone helper reported `Invalid zone catalog pointer` for this fresh process, so zone access
was not accepted by this trial. Further performance work was still under detached native
validation and independent review; this observation does not claim that candidate is live.
