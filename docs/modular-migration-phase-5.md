# Phase 5 migration closure evidence

Status: all source phases and the final boundary are complete on local Main.
Earlier pending-work statements below describe their individual checkpoints; the final
boundary section records current acceptance and the remaining established limits.

Execution follows `docs/superpowers/plans/2026-10-29-modular-architecture-phase-5.md`.
Each section records an actual Main integration and its preserved contract. Private
preparation, final full-boundary gates and original-client acceptance remain distinct.
The whole migration is complete only after every required unit and final gate closes.


## Core presence and lower test fixtures

Core now owns the actual WorldPresence registry/interest algorithm, generic player
projection and shared typed unit-ID helpers. NPC retains population and announcement
policy. The moved projection, add/remove/sight/floor-division and complete Presence bodies
remain exact, including range hysteresis, hidden/dead snapshots, collision/range validation,
metrics and TimedMutex scopes. State/session/listener constructors, close/map/timer and
Handshake bodies remain unchanged. Native's public header reexports the same names.

A single MemoryRepositories test aggregate moves to
`persistence/tests/memory_character_repositories.hpp`; its old testing header becomes a
compatibility reexport. Namespace, constructor/member order, shared MemoryDatabase and
repository methods remain token-identical. The economy test helper changes only its
owning include/preamble. Core tests use persistence rather than the broad testing target;
unrelated legitimate testing consumers remain intact. This removes the indirect
Housing/Loot/NPC closure as well as the original direct upper-layer test edges.

Actual Main ALL Release builds every consumer at three jobs without source repairs.
All 1943 names are unchanged and unique; exactly two PlayerWireUnitId cases move from
NPC to Core. All 1645 prior names, 116 native and 16 graduation definitions remain exact.
Core's 76 definitions become 78 and NPC's 103 become 101. Complete shared-fixture
consumer sets also remain exact: Shop 54, Market 18, Mail 26 and Housing 80.

The primary 312-case gate passes 310 with two original PostgreSQL skips. A disjoint
97-case Market/Housing supplement passes 94 with three original PostgreSQL skips.
Combined: 409 distinct cases, 404 passes, zero failures and five existing optional skips.
All 78 Core cases pass without skips, including the real catalog-bound sector case;
36 route/pipeline and 19 admission/presence cases pass. The newly linked server passes
World 39 and Market 7 smoke checks. No controlled-seven rerun was needed for this unit.

The actual Core test link uses Core plus auth, common, data, distribution, messages,
persistence and protocol archives and established external libraries. It has no testing,
NPC, feature or session runtime archive. The executable's output import-library option
is explicitly separated from dependency inputs in that inspection. Module and whitespace
checks pass. All 17 applied hashes match the sealed candidate and peer review; all other
source bytes, including MapEntry style, bus and factory, remain exact. Scoped formatting
is reviewed; three inherited Presence-only wrapping differences are disclosed rather than
claimed as whole-file formatter compliance.

Evidence is under `out/modular-plan/presence-integration`. Private Core 78/78 and independent
source/guard review support preparation; the Main results above establish integration.
No gameplay, schema, packet/feature status, deployment or new original-client acceptance
change is claimed. Neutral composition, named capabilities/WorldState, final owner/style
closure and focused GM ports still require their ordered Main integrations and final gates.


## Neutral event observers and explicit session lifetime

SessionEventObservers now supplies the application effects directly; the event factory
no longer accepts a GM command context. All 13 subscriber positions are unchanged.
Protection, visibility, combat snapshot, company membership and progress adoption use
five required named slots; eight existing optional slots retain their semantics. The
five generic GM sink declarations disappear, while the actual Native effect bodies
remain unchanged after parameter substitution. The old GM TEST bodies stay intact
through a test-local compatibility factory, not restored production coupling.

Declaration and construction order is Handshake, bus, 25 player-owner fields, then
pipeline. Destruction reverses that order. This is 28 named components, not 28 bus
consumers. Observer construction only captures this and validates slots; it neither
publishes nor reads later unconstructed owners. Existing queued strong-self/alias
captures and current-owner lookup remain. Current-model/stat/build copies are exact:
borrowed references do not escape synchronous publish, while the original Handshake
cache still makes its owning copies. No inspected feature destructor publishes.

Moving bus allocation/required-slot validation earlier changes constructor-error
precedence; the report does not claim identical allocation-failure ordering. State and
listener construction, close/map/timer/Handshake/process_frame bodies remain exact;
no manual reset sequence or new callback scheduling, payload copy, catch or implicit
lock was introduced. Cross-player hubs remain the existing delivery mechanism.

Actual ALL Release builds every affected consumer at eight jobs without source/resource
repairs. All 1943 prior names remain plus four (1947 unique), with no loss, duplicate or
binary-owner transfer. All 116 original native and 16 graduation definitions, 29 retained
native definitions and 24 old GM definitions stay exact. The focused 377-case run passes
374, fails none and skips three existing PostgreSQL opt-ins in 36.32 seconds. All 51
required cases pass: 24 GM, 16 pipeline, seven bus and four neutral contract cases. The
latter cover required slots, current-target lookup and borrowed/lifetime behavior.
Selected real lifecycle/current-model/social checks, 36 routes and 19 admission cases
pass, as do rebuilt-server World 39 and Market 7 smoke checks.

All ten applied payloads match the frozen plan and all other source bytes remain exact,
including Core owners and MapEntry style. Module and whitespace checks pass. All eight
non-Native C++ payloads match their scoped formatter evidence; no whole-Native formatter
claim is made. No controlled-seven rerun was needed for this unit. Evidence is under
`out/modular-plan/neutral-integration`; private 51/51 preparation is kept separate.
Named capabilities/WorldState, final owner/style and focused GM units and final Phase 5
gates remain. No gameplay, schema, packet/status, deployment or new client acceptance
change follows.


## Named Nuia and Gathering capabilities and State repository closure

Nuia no longer exposes the codec mutex through its feature context. The named Native
operation takes the same TimedMutex and publishes the same borrowed Character/stats
snapshot; growth still runs after release. No recomputation, new subscriber lock or
payload copy is introduced. Gathering similarly delegates locked registry visitation
to Native. Sender exclusion, weak lookup, map eligibility, strong peer retention, visitor
encoding, per-packet catches and partial delivery survive at their original positions.
The retained sends still happen after unlock.

The caller's std::function conversion now occurs before lock acquisition, and its temporary
is destroyed after unlock; previously both were inside the feature's lock scope. This
can change allocation-failure timing, although no peer/gameplay operation has yet occurred.
No allocation experiment is claimed. Per-peer ownership release and retained post-unlock
delivery remain unchanged; the complete four locked operation preimages reverse exactly.

WorldState drops its duplicate Mail, Market and Ranking repository references because
those resources already have real feature owners. It retains the typed Fog pointer at
the original position. The public startup DTO, null guards and Ranking/Market/Mail/Fog
installation order are unchanged. Observable shared-pointer use counts decrease; the
actual final resource lifetime is retained by feature owners that outlive the old duplicate
container. This documents set-once startup composition and introduces no live replacement
or concurrent setter policy. The existing supported delivery installation order remains.

Actual Main ALL Release passes at eight jobs (38 actions) without source/resource repairs.
All 1947 names, owners and test properties stay exact, including all 1645 pre-Phase-4
names, 116 original native and 16 graduation definitions, 29 retained Native definitions,
and all 19 Nuia/10 Gathering bodies. The focused 356-case run passes 354, fails none and
skips two existing PostgreSQL opt-ins in 35.61s. The 29 complete owner cases pass, including
the real-memory tool-wear refusal and separate successful-port ordering. The latter does
not convert that real persistence refusal into an implemented successful durability path.
Routes/pipeline 36, admission 19 and selected real native/current-model/Core/GM/Mail/Market
and repository-injection consumers pass. The rebuilt server passes World 39 and Market 7;
complete Mail 26 and its owning concurrent TLS cases already cover the setter change,
so a duplicate Mail smoke run was not needed for this unit.

All seven applied hashes match the sealed candidate. Constructors, owner/bus declarations,
close/map/timers/Handshake/process_frame and neutral observers are byte-exact; all other
source bytes, Core owners and MapEntry style remain unchanged. The six owner/fixture files
match formatter evidence; Native has scoped token/format review. Module and whitespace
checks pass. No controlled-seven rerun or gameplay/test/gate repair was used. Evidence is
under `out/modular-plan/capability-integration`; the private 29/29 preparation and independent
bridge/lifetime reviews remain distinct. Final owner/style, focused GM and full Phase 5
boundary checks still remain. No gameplay, schema, packet/status, live database, deployment
or new original-client acceptance change is claimed.


## Final Expression and GM policy ownership and source cleanup

ExpressionSession now owns the exact former Native GM emotion-grant classification,
malformed usage reply and successful grant body. Unmatched requests return nullopt;
malformed grants are claimed without creating the lazy player. Existing body-routing,
progress reads, durable save/adoption and reply order are unchanged. Native retains only
the named owner call; its newly unused private grant forwarder disappears.

GM's online-player line builder contains the identical name/flag/map expression and is
called at the same vector append inside the same codec-locked directory loop. Lookup,
eligibility, ordering and strong peer collection remain exact; the local result survives
every reply and exception unwind. No audience/lock/catch/allocation position changes.

Guarded cleanup removes 16 primitive constants proven to occur only at their declarations,
orphaned comments and excessive blank runs; public APIs/reexports and the live graduation
adapter remain. The Core comment now correctly describes owning JSON event copies.
A separate Root plan changes only two Nuia comments to state its real broad-guard/lazy
invariant, keeping line count and all other bytes exact. The original eight-path candidate
and this ninth comment path retain separate sealed application receipts.

Actual Main ALL Release passes at eight jobs (102 actions), relinking public-header and
application consumers without source/resource repair. The focused 303 checks pass 302,
fail none and skip one existing EmotionStorage PostgreSQL opt-in in 34.41s. All 12
Expression cases pass, including three new unmatched/malformed/current-progress/order
seams; no simple string-helper or style test was added. Routes/pipeline 36, admission 19,
selected real GM/Expression/native/Core/Nuia/public-header checks and rebuilt World39/
Market7 pass. The 1947 old names remain plus three (1950 unique), with no owner moves,
losses or duplicates. All 1645 prior names, 116 original native, 16 graduation, 29 retained
Native and nine old Expression definitions remain exact.

Independent identifier/operator/literal token lists on actual Main reverse the two moved
policies, one unused forwarder and 16 declarations, with every other executable token
exact. The full locked GM loop, public API/default behavior and Core movement tokens
are conserved. All eight frozen outputs plus the explicit Nuia comment output match;
all other source bytes, MapEntry, bus and graduation adapters are unchanged. Module,
whitespace and scoped formatter checks pass. Whole-file previews identified two inherited
single-line lambdas for a separately guarded token-identical expansion after focused GM,
before final boundary builds; no whole-file formatter claim hides that remaining step.

Evidence is under `out/modular-plan/finishing-integration`, with independent Root review
under the private finishing package. No controlled-seven rerun, gameplay/assertion/gate
repair, schema/packet/status change, live database or deployment action was performed.
Focused GM integration and full final Phase 5 gates remain.


## Focused GM command capabilities and compatibility

GmSession now consumes eight cohesive capability roles rather than the 91-method
aggregate: session/runtime 15, combat 25, character 13, social 11, quests four, world 15,
mail three and delegated feature dispatch five. All operations are assigned exactly once.
The constructor accepts a named-reference bundle by value and stores eight direct
references; it does not retain a broad context or a reference to a temporary bundle.
Production WorldSession directly implements and explicitly binds those roles at the
same GM initializer position. Distinct providers are supported without lookup, a new
bus/dispatcher, command executors or per-method forwarding layers.

The old GmSessionContext and constructor/factory call shapes remain explicit compatibility
adapters. Their delegators bind references only, with no virtual operation, allocation
beyond the existing make_unique, lookup, lock, publication or scheduling. Production
cannot select that old aggregate overload. Internal virtual tables/object layout change,
so all affected Main consumers are rebuilt and relinked; private archive evidence alone
would not establish acceptance.

Strong identifier/operator/literal token lists reverse all 190 ordered calls and conserve
all 91 declarations. Every Native adapter body is byte-identical. The ten mutex scopes,
two retained-owner async sites, current borrowed views/copy behavior and strong aliasing
GmPeer/result ownership through replies/unwind remain. All other Native constructor,
member-order, bus/lifecycle/close/map/timer/handshake behavior is exact. Four lexer negative
controls distinguish merged identifiers, operator boundaries and changed string content.

Actual ALL Release passes at eight jobs, including all changed-ABI consumers and the
server, without source/resource repair. The focused 404-case gate passes 400, fails none
and skips four existing PostgreSQL opt-ins in 36.85s. All 52 required cases pass: 24 old
GM, 16 pipeline, seven bus, four neutral and one meaningful distinct-provider scenario.
It exercises actual commands across separate providers, returned packets, deadlines,
replies and effects; no per-method mirror tests were added. Safe real native/peer/mail/
social/lifecycle/Core consumers, routes/admission and rebuilt World39/Market7 pass.
The 1950 old names remain plus one (1951 unique), with no owner moves/loss/duplicate;
all original native/graduation/retained definitions and old GM24 bodies remain exact.

All seven outputs and finishing/read contracts match the sealed plan; Nuia's separate
Root comment and MapEntry style remain intact. Module, whitespace, scoped formatter and
ABI/link evidence pass. The separately prepared two-lambda layout overlay is still held
for the final boundary; this unit matches frozen Native18ea exactly. Evidence is under
`out/modular-plan/gm-capability-integration`, with private 52/52 and independent peer
review kept distinct. No controlled-seven rerun, gameplay/assertion/gate change, live
DB action, deployment or new original-client acceptance is claimed. Full final Phase 5
boundary checks still remain.


## Final migration boundary and remaining limits

All ordered source units and the final migration boundary are complete on local Main.
Phase 2 separated the eight repositories, Phase 3a introduced the ordered frame pipeline,
Phase 3b moved TLS/connection transport to net, and Phase 4 established all 28 real feature
owners. Phase 5 commits are Core 3601ff7, neutral composition 5b3c17f, capabilities/State
969c9ef, finishing 3110d27 and focused GM 39ada27. The final separate style overlay expands
two lambdas without changing their executable token lists. The live graduation adapter
and public compatibility names remain because actual callers still use them.

Actual final Main matches all 610 composed source/header/CMake hashes and 43 final
contracts. Independent ownership review covers all 28 modules, bootstrap, State, event
composition and the seven earlier Important findings. Required ownership repairs are
closed. Existing standalone delivery-setter misuse, callback/destructor constraints,
other codec-scope conventions, real tool-durability refusal and prior client limits remain
explicit; this refactor does not implement new gameplay or general API hardening.

The Release ALL build passes at eight jobs. The full default run has 1832 passes, zero
failures and 119 skips across 1951 unique registrations. All 1943 Phase 4 names remain plus
exactly eight reviewed new cases, with no removed or duplicate names. The earlier 1645-name
Phase 3b baseline and all 116 original Native/16 graduation bodies are conserved; documented
owner transfers remain recorded in the ledger. All 78 actual Core cases pass across default and applicable fixture runs.
The 64 current-fixture cases pass without skips; the separate EncodeOnce benchmark passes.
The AccountSession registration is covered by its actual child invocation inside passing
integration.distribution. World39, Market7 and Mail12 pass in the real full-suite run.

The controlled seven execute without skips and reproduce their saved assertion/exception
families: raw CTest exit 8 remains a failure, not a passing test run. The unchanged strict
record is retained alongside the narrow namespace/repository-alias annex; seven saved
positive and 31 rejected negative controls pass. No expectation, assertion, timeout, skip,
fixture or gameplay change was used. There are 1898 successful unique cases including the
nested AccountSession case, seven actual established failures, and 46 optional limitations:
34 PostgreSQL fixture cases, nine historical/capture cases and three launcher backend
fixtures. These partitions cover exactly 1951 registrations; not every registered test
passed or ran. The remaining optional inputs are not replaced with current generated data.

Fresh replay after canonical clean/rebuild matches the final executable/dependency and
protocol identities. It compares 17055 records successfully, with 4185 JSON-format-only
differences. The existing 753-byte incomplete capture tail remains disclosed; no strict-tail
or new original-client acceptance is claimed. The existing Fog restart smoke passes in a
fresh disposable loopback PostgreSQL cluster, without a live database URL. That tool retains
its diagnostic child OPENSSL_ia32cap=0; default/current/smoke/replay evidence uses normal CPU
dispatch. No live database, service, client or deployment change was made.

All four canonical build scenarios pass on the existing cached directory: clean 260.4s,
market source 12.0s, market header 19.1s and shared character-state header 198.5s. The
canonical child returns zero in 491.26s. Actual Ninja uses its unchanged default 26 jobs on
24 logical processors; private compilers are paused and existing applications remain.
Observed overall CPU load is 34 percent before and 26 percent after. Both previous Ninja
logs are preserved, all 610 source contents remain identical, all three touched timestamps
are restored, and build-performance.md changes only by append. The feature-source edit
meets the under-one-minute goal. The 3x clean-build target is not met in this loaded run;
comparison with the original 538.7s baseline is not a matched performance experiment.

Two evidence-only preflight corrections preserve their original failures and sealed
predecessors. A narrow new-case locator recognizes GTest's second-line location for one
wrapped TEST declaration while retaining exact name, executed filter, owner, whole-file,
body and macro-span checks. Eight actual positives and eleven negative controls pass;
15 completed build/default records are reused byte-for-byte, without rerunning the runtime
work. A read-only limited process-image query handles missing Windows metadata; only the
previously identified hardware utility's protected parent may record denied image metadata,
with exact fresh parent creation time. ReSharper parent/path/hash/idle checks, descendant
checks and rejection of unreviewed compilers remain. Both corrections receive independent
read-only peer review. They do not change runtime source or test acceptance.

Evidence: out/modular-plan/final-phase5-boundary-integration,
final-phase5-gate-39ada27e-corrected-ledger, final-phase5-postclean-replay-39ada27e and
final-phase5-timing-39ada27e. Original unsuccessful accounting and protected-metadata
preflights remain available. Independent source, accounting and timing reviews are in the
private checkout's final-phase5-ownership-audit, final-phase5-accounting-peer-review and
final-phase5-timing-peer-review packages. Feature classifications, packet overrides,
schema and preserved contracts remain unchanged. Completion covers the local source migration
and its recorded checks. Existing gameplay and original-client acceptance limits remain;
deployment and branch publication are separate.

## Follow-up hardening of the frame pipeline and feature deliveries

A behavior-preserving follow-up closes the review findings left after the migration.
The world request IDs shared by the frame pipeline, native world and their owning features
(skills, quests, chat, revive, escape, adventure objects, interaction, equipment, map load and
zone channel move) are now defined once in `world/core/world_request_opcodes.hpp`. Features
that published their own copy of one of these IDs now alias the shared definition, and the
numeric values are pinned by a test. Range-wide route assertions now cover the loot, revival,
housing, chat, companion/trade, item-use, company, equipment, progression, merchant,
processing, gathering, utility-bag and private-combat handlers. Their owners keep stateful
ownership predicates, so those routes are checked against the opcode constants each owner
publishes instead of against live sessions. Six literal traces pin the complete ordered list of
pipeline steps for an unknown opcode, TimeSync, MapLoadComplete, interaction start, escape and
a frame without a gameplay request.

Native world statically ties its credential slice sizes (packet key, prefix, token and the
transport tail) to the transport constants. Party, gathering and guild deliveries keep their
behavior when a delivery is installed; when none is installed, a non-empty computed update is
dropped as before but reported once with an error log, and each feature has unit coverage for
the dropped, empty and delivered cases.

No gameplay, packet status, persistence, ordering or limit changed. The default Release suite
has zero failures; evidence is under `out/modular-plan/followups`.
