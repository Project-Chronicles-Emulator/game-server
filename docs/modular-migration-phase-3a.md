# Phase 3a: ordered world-frame pipeline

The frame pipeline replaces the orchestration inside `WorldSession::process_frame`.
Feature methods remain on `WorldSession` for the next extraction phase. TLS listener
and connection ownership are unchanged by this phase.

`world/core/frame_router.*` compiles each declared opcode's ordered route once per
session. Observers retain their positions among handlers and continue after a request
is consumed. A handler returning false permits later handlers to inspect the same
request. Explicit broad handlers preserve unknown-opcode tutorial work and the original
statue/escape lazy-construction checks. The router owns diagnostic names and routing
metadata; its borrowed operations outlive it.

`world/session/frame_pipeline.cpp` is the composition point. Its 54 computation steps
contain 22 observers and 32 handlers, including the non-consuming tutorial tail. The
adapter retains 31 consuming branches and two paired position invalidations. The 11
delivery/postprocessing operations remain after the main rejection boundary, with the
same five narrower recovery scopes. The response remains separate from notifications
until the existing I/O continuation queues it first.

The worker strand, protocol lock, metrics scopes, feature guards, shared-service
synchronization, deadline units and stream continuation retain their previous roles.
The pipeline is the last session member, initializes before the active-connection
increment and is destroyed before the fields its operations borrow. No gameplay,
storage schema, packet status or original-client compatibility claim changes.

## Verification

On completed Phase 2 main, the Release build passed. The complete CTest registration
grew from 1,577 to 1,613 unique names: no existing name was removed and all 36 additions
are the 20 router and 16 pipeline cases. The full run had 1,494 passes, 119
environment-gated skips and zero failures in 73.30 seconds.

The existing native integration test source remains unchanged, including all 116 test
definitions and assertions: 112 NativeWorld, three admission and one registry case.
The four existing core test sources and graduation test source are also unchanged.
The module include checker reports zero known violations. The native-world smoke's
39 synthetic checks passed; this does not establish original-client acceptance.

A separate fixture-enabled run in the isolated checkout exercised the generated
catalog, tutorial profile and NPC placements. Across core, session and listener tests,
222 cases passed, seven failed and three PostgreSQL opt-ins skipped. All 36 new cases
passed. The single hardcoded catalog test also passed using a temporary read-only
catalog junction, which was removed afterward. Original fixture hashes were unchanged.

The seven failures were compared against the saved pre-pipeline source with identical
inputs, timeouts and parallelism. Every failure reproduced with the same assertion or
exception signature. The candidate source was restored byte-for-byte and rebuilt.
These are baseline failures for those selected inputs; their underlying causes remain
unresolved. No test was weakened or gameplay behavior changed to make them pass.

| NativeWorld test | Reproduced baseline signature |
| --- | --- |
| InPlaceReviveIsDurableRepeatableAndIgnoredForALivingOrRepeatRequest | Frame-read timeout |
| PlayerWireUnitIdHasThePlayerTypeNibbleForRawCharacterIds1And17And19 | Frame-read timeout |
| OverworldAndTutorialSessionsStayIndependentOnOneServer | Response assertions and missing shop_id |
| TradeRunFromWatchersPostToSeaglassHarborPaysTheRouteReward | Requested NPC absent from placement fixture |
| FallDamageReachesSocketAndLethalLandingPersistsThroughReconnect | Frame-read timeout |
| SharedMapFallDamagePersistsDeathAndSupportsRevival | Missing revival packet and retained dead state |
| SharedMapDrowningTimerPersistsDeathAndRevivalClearsTheDive | Retained dead state |

The fixture-enabled run also included the separately prepared listener extraction;
the current-main full run above contains Phase 3a alone. Detailed private receipts,
comparison scripts and source fingerprints are under ignored
`out/modular-plan/phase3a-integration` and the isolated checkout's
`out/modular-plan/pipeline-integration`. Build measurements are recorded in
`docs/build-performance.md`.
