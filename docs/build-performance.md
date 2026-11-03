# Build performance

Measurements from `tools/measure_build.ps1` for the modular architecture work
(`docs/superpowers/specs/2026-10-27-modular-emulator-architecture-design.md`). Each record
times a clean Release build and three incremental rebuilds on the same machine. Compare
records only when the machine and its other load are similar.

Design targets: a one-file feature change rebuilds in under 60 seconds, and a clean build is
at least 3 times faster than the baseline record.

## Baseline: Visual Studio generator, flat src/

Measured 2026-10-28 00:31 on commit `9995eca` with Visual Studio 18 2026, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 538.7 |
| Touch one feature source (market.cpp) | 31.7 |
| Touch one feature header (market.hpp) | 59 |
| Touch the shared storage header (storage.hpp) | 249.2 |

## Phase 0: Ninja Multi-Config, bin/ outputs, precompiled headers, flat src/

Measured 2026-10-28 01:16 on commit `34775c6` with Ninja Multi-Config, preset `release`, 24 logical processors.

The measured tree already contained the precompiled-header change later committed as `4cb5b33`, plus another session's uncommitted zone-unlock edits (committed later as `2141a6d`).

| Scenario | Seconds |
| --- | ---: |
| Clean build | 279.8 |
| Touch one feature source (market.cpp) | 14.3 |
| Touch one feature header (market.hpp) | 41.4 |
| Touch the shared storage header (storage.hpp) | 189.6 |

Slowest compile steps in the clean build:

- `src/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 145.3 s
- `src/CMakeFiles/chronicles-tests.dir/Release/native_world_tests.cpp.obj`: 98.3 s
- `src/CMakeFiles/chronicles-world.dir/Release/native_world.cpp.obj`: 82 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 68.1 s
- `src/CMakeFiles/chronicles-world.dir/Release/combat.cpp.obj`: 62 s
- `src/CMakeFiles/chronicles-tests.dir/Release/guild_tests.cpp.obj`: 44.5 s
- `src/CMakeFiles/chronicles-admin-api.dir/Release/admin_api.cpp.obj`: 44.2 s
- `src/CMakeFiles/chronicles-tests.dir/Release/tests.cpp.obj`: 43.9 s
- `src/CMakeFiles/chronicles-tests.dir/Release/native_auth_tests.cpp.obj`: 43.7 s
- `src/CMakeFiles/chronicles-auth-flow-tests.dir/Release/native_auth_tests.cpp.obj`: 43.7 s

## Phase 1a: module folders, existing targets

Measured 2026-10-28 02:15 on commit `85542ca` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 222.5 |
| Touch one feature source (market.cpp) | 11.8 |
| Touch one feature header (market.hpp) | 29.4 |
| Touch the shared storage header (storage.hpp) | 132.9 |

Slowest compile steps in the clean build:

- `src/CMakeFiles/chronicles-world.dir/Release/world/session/native_world.cpp.obj`: 135.4 s
- `src/CMakeFiles/chronicles-server.dir/Release/apps/server.cpp.obj`: 125.4 s
- `src/CMakeFiles/chronicles-tests.dir/Release/world/session/tests/native_world_tests.cpp.obj`: 89.3 s
- `src/CMakeFiles/chronicles-tests.dir/Release/world/features/combat/tests/combat_tests.cpp.obj`: 73.6 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 61.2 s
- `src/CMakeFiles/chronicles-world.dir/Release/world/features/combat/combat.cpp.obj`: 49.9 s
- `src/CMakeFiles/chronicles-tests.dir/Release/testing/tests.cpp.obj`: 44.1 s
- `src/CMakeFiles/chronicles-vehicle-tests.dir/Release/world/features/companion/tests/companion_tests.cpp.obj`: 42.7 s
- `src/CMakeFiles/chronicles-admin-api.dir/Release/services/admin/admin_api.cpp.obj`: 41.9 s
- `src/CMakeFiles/chronicles-admin-tests.dir/Release/services/admin/tests/admin_tests.cpp.obj`: 41.9 s

Against the Visual Studio baseline, the clean build is 2.4x faster (538.7 s to 222.5 s; target: 3x, not met; phase 1b may reduce recompilation further, but that is an unverified expectation, not a measured result) and a one-file feature change (market.cpp) rebuilds in 11.8 s (target: under 60 s, met).

These are single runs. An earlier phase 1a run on the same sources took 307.3 s for the clean build, so single-run clean-build differences of this size are not conclusive. Future ratio claims should use the median of at least three runs. The slowest units (`native_world.cpp`, `server.cpp`) only shrink when phases 3-4 split them.

## Phase 1b-2: module libraries and test programs

Measured 2026-10-28 16:08 on commit `82fc153` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 200.5 |
| Touch one feature source (market.cpp) | 8.6 |
| Touch one feature header (market.hpp) | 26.5 |
| Touch the shared storage header (storage.hpp) | 152.6 |

Slowest compile steps in the clean build:

- `src/apps/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 70.6 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat-tests.dir/Release/tests/combat_tests.cpp.obj`: 54.9 s
- `src/world/session/CMakeFiles/chronicles-world-session.dir/Release/native_world.cpp.obj`: 50.8 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 45.8 s
- `src/services/agent_engine/CMakeFiles/chronicles-services-agent_engine.dir/Release/agent_engine_http.cpp.obj`: 40 s
- `src/world/session/CMakeFiles/chronicles-world-session-tests.dir/Release/tests/native_world_tests.cpp.obj`: 36.4 s
- `src/auth/CMakeFiles/chronicles-roster-tests.dir/Release/tests/native_auth_tests.cpp.obj`: 33.3 s
- `src/world/features/loot/CMakeFiles/chronicles-world-features-loot-tests.dir/Release/tests/loot_tests.cpp.obj`: 30.9 s
- `src/world/features/character/CMakeFiles/chronicles-world-features-character-tests.dir/Release/tests/character_stats_tests.cpp.obj`: 29.9 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat.dir/Release/combat.cpp.obj`: 29.7 s

Against the phase 1a record, the clean build is a little faster (222.5 s to 200.5 s), a one-file
feature change (market.cpp) rebuilds in 8.6 s (11.8 s) and a feature header (market.hpp) in
26.5 s (29.4 s), while the shared storage header rebuilds in 152.6 s (132.9 s): a
`storage.hpp` change now also relinks the 37 of the 43 per-module test programs that link
`chronicles-persistence`. These are single runs with the known spread (an earlier phase 1a
clean build took 307.3 s), so differences of this size are not conclusive. Against the
Visual Studio baseline the clean build is 2.7x faster (538.7 s to
200.5 s; the 3x target is not met — the slowest units `server.cpp` and `native_world.cpp`
only shrink when phases 3-4 split them). The measured tree carried this phase's uncommitted
Task 7 changes (include-checker rules and `tools/source_layout.json`); they touch no build
input.

## Phase 2: storage split into repositories

Measured 2026-10-28 23:42 on commit `b9bf08f` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 317.9 |
| Touch one feature source (market.cpp) | 16.4 |
| Touch one feature header (market.hpp) | 51.6 |
| Touch the shared character-state header (character_state_types.hpp) | 228.3 |

Slowest compile steps in the clean build:

- `src/apps/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 130.3 s
- `src/world/session/CMakeFiles/chronicles-world-session.dir/Release/native_world.cpp.obj`: 121.7 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 56.6 s
- `src/world/session/CMakeFiles/chronicles-world-session-tests.dir/Release/tests/native_world_tests.cpp.obj`: 56 s
- `src/services/agent_engine/CMakeFiles/chronicles-services-agent_engine.dir/Release/agent_engine_http.cpp.obj`: 50.4 s
- `src/auth/CMakeFiles/chronicles-roster-tests.dir/Release/tests/native_auth_tests.cpp.obj`: 42.5 s
- `src/auth/CMakeFiles/chronicles-auth-flow-tests.dir/Release/tests/native_auth_tests.cpp.obj`: 41.1 s
- `src/world/features/loot/CMakeFiles/chronicles-world-features-loot-tests.dir/Release/tests/loot_tests.cpp.obj`: 40.7 s
- `src/world/features/character/CMakeFiles/chronicles-world-features-character-tests.dir/Release/tests/character_stats_tests.cpp.obj`: 38 s
- `src/world/features/companion/CMakeFiles/chronicles-vehicle-tests.dir/Release/tests/companion_tests.cpp.obj`: 36.7 s

Every scenario in this run is slower than the phase 1b-2 record (clean 317.9 s vs 200.5 s,
market.cpp 16.4 s vs 8.6 s, market.hpp 51.6 s vs 26.5 s). The game client and a screen
recorder were running throughout this measurement (overall CPU load about 80 % before the
build started), while the phase 1b-2 record was taken without them, so these deltas say
nothing about the split itself. Re-measure on an idle machine before comparing; these
remain single runs with the known spread (an earlier phase 1a clean build took 307.3 s).
Against the Visual Studio baseline the clean build is 1.7x faster this run (538.7 s to
317.9 s; best recorded ratio is still the phase 1b-2 run's 2.7x; the 3x target is not met —
`server.cpp` and `native_world.cpp` still dominate and only shrink when phases 3-4 split
them). The shared-header scenario now touches `character_state_types.hpp` instead of the
deleted `storage.hpp`: the types header is transitively included by 164 of the 220
translation units (storage.hpp reached 162 of 216), because `Character`, `WorldProgress`
and `ItemTrade` remain shared vocabulary for most of the tree. The structural change is
that the repository interfaces are far narrower: `character_repository.hpp` reaches 120 of
220 TUs, `account_repository.hpp` 98 and the aggregate `character_repositories.hpp` 76, so
editing a repository contract no longer rebuilds the market/mail/hotkey/fog features that
previously inherited all of `storage.hpp`.

## Phase 3a: ordered frame-processing pipeline

Measured 2026-10-29 00:50 on commit `36ecea9` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 347.6 |
| Touch one feature source (market.cpp) | 13.6 |
| Touch one feature header (market.hpp) | 39.9 |
| Touch the shared character-state header (character_state_types.hpp) | 251.7 |

Slowest compile steps in the clean build:

- `src/world/session/CMakeFiles/chronicles-world-session.dir/Release/native_world.cpp.obj`: 157.4 s
- `src/apps/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 125.4 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat-tests.dir/Release/tests/combat_tests.cpp.obj`: 104.1 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 86.8 s
- `src/world/session/CMakeFiles/chronicles-world-session-tests.dir/Release/tests/native_world_tests.cpp.obj`: 77.2 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat.dir/Release/combat.cpp.obj`: 71.9 s
- `src/world/features/loot/CMakeFiles/chronicles-world-features-loot-tests.dir/Release/tests/loot_tests.cpp.obj`: 68.1 s
- `src/world/features/tutorial/CMakeFiles/chronicles-world-features-tutorial.dir/Release/tutorial.cpp.obj`: 61.4 s
- `src/world/features/gm/CMakeFiles/chronicles-world-features-gm-tests.dir/Release/tests/game_master_tests.cpp.obj`: 60.1 s
- `src/world/session/CMakeFiles/chronicles-world-session-tests.dir/Release/tests/frame_pipeline_tests.cpp.obj`: 57.4 s

The measured tree includes the Phase 3a pipeline changes on top of `36ecea9`; the
reported commit is the clean Phase 2 publication base because the phase commit follows
its verification. Both the original game client and OBS were running during this run.
Other migration agents deferred compilation until these measurements finished. These
are loaded-machine single runs, so they do not establish a speedup or regression against
the earlier records. In particular, feature bodies remain in `native_world.cpp` in this
phase; moving them into their owning modules is Phase 4 work. All four measured build
commands completed successfully.

## Phase 3b: native world transport extraction

Measured 2026-10-29 01:18 on commit `980f838` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 353.3 |
| Touch one feature source (market.cpp) | 14.1 |
| Touch one feature header (market.hpp) | 33.1 |
| Touch the shared character-state header (character_state_types.hpp) | 249.6 |

Slowest compile steps in the clean build:

- `src/apps/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 130.6 s
- `src/world/session/CMakeFiles/chronicles-world-session.dir/Release/native_world.cpp.obj`: 105.7 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 101.5 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat-tests.dir/Release/tests/combat_tests.cpp.obj`: 100 s
- `src/world/session/CMakeFiles/chronicles-world-session-tests.dir/Release/tests/native_world_tests.cpp.obj`: 75 s
- `src/world/features/guild/CMakeFiles/chronicles-world-features-guild-tests.dir/Release/tests/guild_tests.cpp.obj`: 71.8 s
- `src/world/features/loot/CMakeFiles/chronicles-world-features-loot-tests.dir/Release/tests/loot_tests.cpp.obj`: 69.2 s
- `src/services/agent_engine/CMakeFiles/chronicles-services-agent_engine.dir/Release/agent_engine_http.cpp.obj`: 66.4 s
- `src/world/features/loot/CMakeFiles/chronicles-world-features-loot-tests.dir/Release/tests/loot_custom_tests.cpp.obj`: 65.1 s
- `src/auth/CMakeFiles/chronicles-roster-tests.dir/Release/tests/native_auth_tests.cpp.obj`: 58.2 s

The measured tree includes the Phase 3b transport extraction on top of `980f838`; its
phase commit follows verification. The game client and OBS remained running. Small
private feature-preflight compilations were permitted during this run, so background
load was not controlled. All four build scenarios completed successfully. These single
loaded-machine runs do not establish a speedup or regression against the earlier
records. Most feature bodies remain in `native_world.cpp` until Phase 4.

## phase4-83f89a9-style-9f1d0685

Measured 2026-10-29 08:37 on commit `83f89a9` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 283.9 |
| Touch one feature source (market.cpp) | 15.7 |
| Touch one feature header (market.hpp) | 22.7 |
| Touch the shared character-state header (character_state_types.hpp) | 211.4 |

Slowest compile steps in the clean build:

- `src/apps/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 81.1 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat-tests.dir/Release/tests/combat_tests.cpp.obj`: 52.1 s
- `src/world/features/gm/CMakeFiles/chronicles-world-features-gm-tests.dir/Release/tests/native_gm_tests.cpp.obj`: 41.9 s
- `src/world/features/events/CMakeFiles/chronicles-event-tests.dir/Release/tests/native_events_integration_tests.cpp.obj`: 41 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 39.5 s
- `src/world/features/inventory/CMakeFiles/chronicles-world-features-inventory-native-tests.dir/Release/tests/native_inventory_tests.cpp.obj`: 38.1 s
- `src/services/agent_engine/CMakeFiles/chronicles-services-agent_engine.dir/Release/agent_engine_http.cpp.obj`: 36.9 s
- `src/world/features/expression/CMakeFiles/chronicles-world-features-expression-tests.dir/Release/tests/native_expression_tests.cpp.obj`: 36.7 s
- `src/world/features/shop/CMakeFiles/chronicles-world-features-shop-tests.dir/Release/tests/native_shop_tests.cpp.obj`: 36.3 s
- `src/world/features/loot/CMakeFiles/chronicles-world-features-loot-tests.dir/Release/tests/loot_tests.cpp.obj`: 35.7 s

This is the Phase 4 boundary source at `83f89a9` plus the token-identical shared
MapEntry fixture layout cleanup. The actual cached Ninja 1.13.2 used its default
26 jobs on 24 logical processors, without a preset or CMake parallelism override.
Private compilation was paused. Existing launcher, admin, server, Cursor, Steam
and PostgreSQL processes remained running; observed overall CPU load was 54%
before and 35% after. The hardware-control utility and idle ReSharper workers
were identified separately from active compilers and left running.

All four canonical build commands succeeded. Both previous Ninja logs were
preserved before the tool's force-move; all 599 source contents remained identical,
and all three touched timestamps were restored. The canonical child returned 0.
Its outer ignored wrapper returned 1 from the stale status of `ninja --help`,
which returns 1; the separate postmeasurement annex preserves and explains both
statuses. No measurement was repeated. Evidence is under
`out/modular-plan/final-phase4-canonical-timing-83f89a9-20261029T112814-c974465e`.

The feature source edit meets the under-one-minute target. This single loaded
run does not establish a matched speedup or regression against earlier records,
and does not establish the 3x clean-build target. Final Phase 5 measurements remain
required after its remaining source closures.

## phase5-39ada27e-style-01055cf0

Measured 2026-10-29 10:46 on commit `39ada27` with Ninja Multi-Config, preset `release`, 24 logical processors.

| Scenario | Seconds |
| --- | ---: |
| Clean build | 260.4 |
| Touch one feature source (market.cpp) | 12 |
| Touch one feature header (market.hpp) | 19.1 |
| Touch the shared character-state header (character_state_types.hpp) | 198.5 |

Slowest compile steps in the clean build:

- `src/apps/CMakeFiles/chronicles-server.dir/Release/server.cpp.obj`: 69.2 s
- `src/world/features/combat/CMakeFiles/chronicles-world-features-combat-tests.dir/Release/tests/combat_tests.cpp.obj`: 44 s
- `src/world/session/CMakeFiles/chronicles-world-session.dir/Release/native_world.cpp.obj`: 41.3 s
- `src/world/features/events/CMakeFiles/chronicles-event-tests.dir/Release/tests/native_events_integration_tests.cpp.obj`: 40 s
- `src/world/features/inventory/CMakeFiles/chronicles-world-features-inventory-native-tests.dir/Release/tests/native_inventory_tests.cpp.obj`: 39.2 s
- `src/CMakeFiles/chronicles-messages.dir/Release/generated/cms.generated.pb.cc.obj`: 34.7 s
- `src/world/features/escape/CMakeFiles/chronicles-world-features-escape-tests.dir/Release/tests/native_escape_tests.cpp.obj`: 33.6 s
- `src/world/features/npc/CMakeFiles/chronicles-world-features-npc-native-tests.dir/Release/tests/native_npc_session_tests.cpp.obj`: 33 s
- `src/world/features/gathering/CMakeFiles/chronicles-world-features-gathering-tests.dir/Release/tests/native_gathering_integration_tests.cpp.obj`: 32.6 s
- `src/world/features/gm/CMakeFiles/chronicles-world-features-gm-tests.dir/Release/tests/native_gm_tests.cpp.obj`: 32.3 s

Final Phase 5 measurement scope: this is one loaded-machine canonical sample in the existing
`out/build/windows` cache, with 24 logical CPUs and the cached Ninja default of 26 jobs.
There was no preset or CMAKE_BUILD_PARALLEL_LEVEL override. Sampled processor load was
34% before and 26% after; existing game, OBS, IDE and service processes remained running,
and all private compilers were paused. The two previously recorded ReSharper MSBuild
nodes were idle, with no compiler descendants. No process was killed.

Both prior Ninja logs were preserved before the canonical force-move. All 610 source
contents remained byte-identical and all three touched timestamps were restored. All
four builds and the canonical child returned zero; no timing was repeated. The initial
background refresh stopped before any clean, log move or timestamp change. Its ignored
recorder correction uses read-only limited-information lookup for the same GCC hardware
utility PID/start/path/hash. Only its already reviewed protected svchost parent has image
metadata marked unavailable with Win32 access-denied 5; its exact current CIM creation
time, PID and name are rechecked and precede the unchanged child. Full ReSharper parent
path/hash/start, descendant and CPU guards remain intact. The failure, repair annex and
fresh proof are retained under `out/modular-plan/final-phase5-harness-repair` and the
final gate evidence; the original sealed preparation remains unchanged.

The 12.0-second feature-source edit meets the under-one-minute target. The 260.4-second
clean build does not meet a 3x reduction from the earlier 538.7-second baseline. These
loaded samples do not establish a matched speedup or regression, or a player-capacity
claim. Canonical evidence is under `out/modular-plan/final-phase5-timing-39ada27e`.
