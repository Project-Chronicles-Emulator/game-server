# Phase 4 feature extraction evidence

Execution follows
`docs/superpowers/plans/2026-10-29-modular-architecture-phase-4.md`.
Each section records a structural change and its actual checks. Feature/packet status
labels and original-client acceptance limits remain independent of extraction progress.

## Events

`EventsFeature` owns ranking definitions/repository access, the raid matchmaker/timer
and the channel-event definitions/board. `EventsSession` owns the player's ranking
budget/runs and sole authoritative raid return point. Eighteen native methods move
into these objects; the native session retains controlled composition capabilities
and the ordered pipeline adapter.

Ranking throttling, authoritative results, tribute transaction/publication ordering,
raid authorization and return-position normalization retain their previous contracts.
Protocol operations use the existing TimedMutex with its original scopes. Raid origin
is installed before travel and copied before a reentrant departure can clear it.
Arrival, eviction, map departure and leave hooks execute at their old positions.
The shared raid timer retains the party strand and weak ownership; delivery does not
create a world/feature lifetime cycle. Progress adoption remains synchronous.

Startup preserves the ranking/raid load order and late optional channel-event load.
The shared catalog-table availability predicate moves unchanged into data; each
feature keeps its own missing/optional/fatal startup policy. No schema, storage
atomicity, opcode, packet order or gameplay policy changes.

Shared native integration fixtures now live in the test-only
`world/session/tests/native_world_test_support.hpp`. Sixteen helper definitions and
ten constants retain their behavior. The umbrella testing target gains no world-session
dependency; owning native test targets explicitly link the application module.
The support step preserves all 116 original test definitions. The event step retains
115 and moves `NativeWorld.RankingsRaidChannelsAndTributesOverTls` intact into the
events test program, preserving its name, assertions, timeouts and environment gate.

Private preparation passed 17 component cases without skips, production binding and
fixture translation-unit compilation, independent source/lifetime/lock/startup review,
and module/format checks.

Actual main-checkout data/events/session libraries, server and event/session/core test
programs built successfully. Registration grew from 1,645 to 1,662 unique names: 17
component cases added, no old name removed or duplicated. The focused run executed all
111 selected cases in 30.10 seconds: 110 passed, zero failed, and one existing
RankingStorage PostgreSQL opt-in skipped. The moved native TLS case passed with the
read-only catalog opt-in in 14.00 seconds. All 36 router/pipeline and 18
admission/presence cases passed. The actual server passed all 39 synthetic native-world
smoke checks; stock-client credential verification remains unestablished.

The final module and whitespace checks passed. Private receipts are under
`out/modular-plan/events-extraction` in the isolated checkout. The initial unsupported
CTest regex selected no cases and is retained as a failed harness attempt; only the
corrected run asserting all 111 executed cases is counted above. No live database,
deployment or new original-client acceptance is claimed.

## GM and synchronous session effects

`GmFeature` owns the reply/announce callbacks; `GmSession` owns 47 moved command,
diagnostic and player-session methods, authorization/rate state, watches, spawned units,
and protection/visibility flags. Named command/query capabilities retain protected session
identity, registry, transport, progression and transaction access. They expose neither
`WorldSession` nor `WorldState`. Result-bearing optional command dispatch preserves the
difference between an unmatched command and a claimed empty refusal.

Role checks, separate generic/social/car budgets, parser priority, map staging and deadlines
retain their previous positions. The codec uses the existing TimedMutex and original lock
scopes. Peer target handles alias the containing session's shared owner; delayed kick and
summon retain that owner on the same executors. Online-player listing retains every locked
peer until its complete reply loop finishes, including exceptional unwinding. The private
review caught and corrected an earlier query-return lifetime difference before application.

The sole `SessionEventBus` is synchronous and typed. Immutable subscriptions are written
explicitly in `frame_pipeline.cpp` before frame processing. Protection, visibility, combat
cache, company membership and committed-progress effects run at their old call sites;
Events' committed-progress bridge uses the same bus. Five GM combat-cache publications
remain inside their original codec locks. Callbacks resolve rebuilt combat/map state when
invoked. Publication copies no subscriber list, acquires no lock, schedules no work, and
propagates the original exception. Borrowed payloads cannot outlive synchronous publication.
Cross-player delivery remains in the existing hubs. Named optional inventory/activity
observers prepare the same composition boundary for later feature extractions.

The existing shared test-only fixture gains the previously shared vehicle/frame helpers.
All 115 pre-GM native definitions remain: 109 retained and six moved intact to
`gm/tests/native_gm_tests.cpp`, including all 80 assertions, names, timeouts and gates.
Independent source review checked the 47 methods, lifecycle/lock boundaries, exact native
test union and composition. Twenty-seven moved bodies are token-identical after declared
capability substitutions; the remaining owner splits were inspected individually.

Private checks passed 24 GM cases, including the peer-retention regression, and seven
unchanged bus cases; native/composition bindings and owning native test units compiled.
Actual main Release core/GM/session/events/Exploration test programs and the real server
then built successfully. Registration grew from 1,662 to 1,693 unique names: 31 added,
zero removed or duplicated. The focused application run executed 166 cases in 26.07
seconds: 165 passed, zero failed, and only
`Exploration.FogStorage.PostgresMergesConcurrentSavesAndSurvivesReopen` skipped behind
its existing database opt-in. Every GM-target case and historical Exploration case was
selected, together with all 36 router/pipeline cases, seven bus cases, 19 admission/presence
cases and the affected Events TLS bridge. All six moved GM TLS cases passed with the
read-only catalog/tutorial/NPC fixtures. All 39 actual native-world smoke checks passed.

Module/include/cycle and whitespace checks passed. Actual application receipts are under
`out/modular-plan/gm-integration`; reviewed private artifacts are under `gm-extraction`
in the isolated checkout. Full-suite and build measurements remain Phase 4 boundary gates.
No protocol, schema, transaction policy or feature-status change is claimed; live databases,
deployment and new original-client acceptance remain outside this refactor's evidence.

## Expression

`ExpressionFeature` owns emotion definitions and the thread-safe selfie registry;
`ExpressionSession` owns the lazy PlayerExpression, observed movement mode and entry
snapshot flag. Catalog loading keeps its original startup position. Creating the feature
session does not read player progress; the first expression request or entry snapshot
retains that lazy read. Busy/dead/riding/ground-mode refusals and observer-before-owner
packet preparation remain unchanged.

The once-only entry bit is set before snapshot generation/encoding and stays set after
failure. WorldCharEmotionsNoti still precedes the guild snapshot. Map-load and I/O-close
hooks clear the shared selfie registry at their former positions; close does not mutate
worker-owned PlayerExpression. Late viewers receive the same selfie notification after
the decorated UnitAdd. GM emotion grants retain plan/commit/adopt/player-init/reply order,
with committed progress delivered through the existing synchronous session bus.

The complete `NativeWorld.EmotesGrantsAndSelfieModeReachNearbyPlayersOverTls` definition
moves to expression with all 27 assertions, timeouts and its catalog gate. Native ownership
changes from 109 retained definitions to 108 plus that unchanged owning test. Nine new
component cases cover lazy initialization, once-only failure, observer order, movement and
registry lifecycle, closed sessions, and successful/refused/throwing grant commits.

Actual main Release expression/session/core/GM test programs and the server built.
Registration grew from 1,693 to 1,702 unique names, preserving every previous name without
duplicates. All 113 selected checks executed in 7.04 seconds: 112 passed, zero failed, and
only the existing `EmotionStorage.PostgresOwnedEmotionsRoundTripAndStaleRevisionsWriteNothing`
database opt-in skipped. The moved TLS case passed with read-only fixtures in 0.729 seconds;
all 36 router/pipeline and 19 admission/presence cases passed. All 39 actual server smoke
checks passed. Source, test-union, module and whitespace review passed; receipts are under
`out/modular-plan/expression-integration`. No packet, persistence, feature-status or new
original-client acceptance change is claimed.

## Escape

`EscapeFeature` owns catalog/default rules; `EscapeSession` owns lazy PlayerEscape state,
the existing I/O timer and its pending deadline. Rule loading remains at its original
startup position. The broad ownership check still constructs PlayerEscape for surviving
unrelated requests; shrine preflight retains its earlier construction point, while the
passive statue query does not construct it. Trading escapes still fall through.

Request handling retains packet replacement, teleport start/completion order, shrine
commit/revival insertion before position adoption, and the final GM protection override.
The timer still posts I/O to worker to I/O, holds the containing session through pending
operations, preserves partial encoded frames after the narrow catch, and rearms before
delivery on early firing. Codec locks and deadline units are unchanged. Close cancels the
timer without resetting worker state; map changes retain the same cooldown/teleport state.

The intact `NativeWorld.OverworldEscapeTeleportsToTheCandidateAndSavesTheDestination`
definition and its owning helper move to escape with all 13 assertions and original
fixture gate. Native ownership changes from 108 retained definitions to 107 plus the moved
test. Twelve new component cases cover the lazy/broad checks, fallthrough, shrine ordering,
partial encoding, timer executors/rearming, closed sessions and cancellation lifetime.

Actual main Release escape/session/core/GM test programs and the server built.
Registration grew from 1,702 to 1,714 unique names: all prior names retained, no duplicates.
Every one of the escape module's 23 cases passed, including the moved TLS case in 0.450
seconds. The expanded run executed 114 cases in 53.54 seconds: 113 passed, zero skipped,
and `NativeWorld.InPlaceReviveIsDurableRepeatableAndIgnoredForALivingOrRepeatRequest`
failed with the existing frame-read socket timeout in 45.886 seconds. The controlled
earlier comparison reproduced that signature in 46.01 seconds with immutable pre-pipeline
native source SHA `ec8a6f0e83aedb0e085cdf019105aef163049cf03990cec24f5ef423908e0eb4`
and 45.82 seconds with the pipeline. This is a source hash, not a Git revision.
Its underlying gameplay/fixture cause remains unresolved; this run is not reported as
entirely green. No assertion, fixture, skip gate or gameplay behavior was changed for it.

All remaining selected router/pipeline, admission/presence, bus, GM and shrine checks
passed, including conflicting-revision revival and NuiFlame teleport completion. All 39
actual server smoke checks passed. Module, source/test-union and whitespace checks passed;
current receipts are under `out/modular-plan/escape-integration`. Controlled historical
receipts remain under the isolated checkout's `out/modular-plan/pipeline-integration`.
No packet, storage, feature-status or new original-client acceptance change is claimed.

## Shop

`ShopFeature` owns the single optional ShopCatalog and its original declaration-order
startup load. Existing required-table checks, absent-table log/refusal and fatal load
exceptions are preserved. Other features query this same immutable catalog. `ShopSession`
owns merchant request parsing/authorization, the authorized-request budget, weight-ceiling
policy, planning, atomic commit and committed-item refresh. Inventory measurement and
server-owned NPC pose remain narrow result-bearing capabilities.

Only authorized requests consume the existing eight-per-second budget or reach storage.
Tutorial cached progress versus stored progress, open/read-only behavior, commit refusals
and packet order remain unchanged. The shared committed-item refresh still removes/saves/
repairs/grants the same rows, refreshes stats once and then updates the handshake snapshot
with the current combat view. GM uses this same real hook. Existing synchronous bus
subscriptions remain explicit in composition; the snapshot sink preserves each caller's
original codec-lock scope and borrows no payload beyond publication.

Thirteen complete merchant native tests move to Shop, retaining every name, assertion,
timeout and environment gate. Generic capacity/weight/equipment/discard and session-lifetime
tests keep their own ownership. Shared merchant fixtures move once into a Shop test-only
header that retained session cases also use. An independent definition comparison proves
all 107 post-Escape native bodies equal the 93 retained after the complete economy chain
plus 13 Shop and one Mail bodies, including all 120 moved assertions. Shop alone leaves
94 retained native definitions. Housing/trade bootstrap payloads stay with their owners.

Actual main Release Shop/core/session/GM/character/market/mail/Trade/vehicle consumers and
the server built; the emote-native program also relinked against the new session. An initial
nonexistent trade-test target was a harness error; the corrected target/build passed without
source or assertion changes. Registration grew from 1,714 to 1,722 unique names, preserving
all prior names without duplicates. The focused run executed 143 cases in 15.31 seconds:
142 passed, zero failed, and only the original
`NativeWorld.PostgresShopBuyAndSellRefusalLeavesPersistedStateUnchanged` database opt-in
skipped. All eight new and 33 legacy Shop cases passed; 12 of 13 moved native cases passed
with the remaining original database gate preserved. Router/pipeline, admission/presence,
GM and emote regressions passed. All 39 actual server smoke checks passed. Module, source
guards, test-union and whitespace review passed; receipts are under
`out/modular-plan/shop-integration`. No schema, storage atomicity, gameplay policy,
feature/packet status or new original-client acceptance change is claimed.

## Market

`MarketFeature` owns the original MarketCatalog startup load, the same shared ShopCatalog
and the narrow market repository. `MarketSession` owns request routing and the single
16-byte random registration scope for the connection; map changes do not recreate it.
Read-only requests do not refresh inventory. Committed requests retain tutorial progress
adoption, current-character reload, the unlocked inventory snapshot refresh and encumbrance
packet order. Combat stats are not recomputed, matching the previous implementation.
Unknown storage outcomes and post-commit failures still reach the original frame failure
path. The same synchronous bus and explicitly composed inventory sink are reused.

Actual main Release market/session/server and affected consumer test programs built.
Registration grew from 1,722 to 1,729 unique names, retaining all prior names and adding
seven component cases without duplicates. No native test changes owner in this step;
the 94 retained native definitions are unchanged. The focused run executed 115 cases in
7.13 seconds: 114 passed, zero failed, and only the original
`Market.PostgresSettlementPersistsAcrossStoreRecreation` database opt-in skipped.
All seven new cases and the remaining market/economy/GM/router/admission/presence/emote
checks passed. The actual rebuilt disposable server passed all 39 world smoke checks and
seven marketplace TLS request/response checks. These are synthetic checks; the smoke
explicitly retains `original_client_verified=false`. Source, module, registration and
whitespace review passed; receipts are under `out/modular-plan/market-integration`.
No packet, transaction, schema, gameplay policy or feature-status change is claimed.

## Mail

`MailFeature` owns the narrow repository, existing tutorial-completion safe delivery,
online owner selection and recipient fanout. `MailSession` owns the existing worker-side
claim, receipt/cache/notification/completion order. Composition retains worker posting
and the containing session. Owner/account/world filtering still precedes lazy in-world
queries; selected callbacks retain the same peers into posting after the codec lock.

Claim guards, durable receipt and tutorial adoption are unchanged. The snapshot publication
remains inside its original codec lock, followed by encumbrance, encoding, ordered posting
and completion. Combat stats are not recomputed. An uncertain commit or post-commit failure
still requests the original admin disconnect and reports conflict. GM retains authorization,
its separate durable-versus-notification catches and its original explicit character-mail
wire variant; ordinary fanout retains account/character selection. Tutorial still owns
the completion trigger and its postlude placement. No duplicate delivery path is added.

The complete concurrent-claims native TEST moves intact to Mail; 94 retained definitions
become 93 plus that unchanged test. Actual main Release mail/session/server and affected
consumer tests built. Registration grew from 1,729 to 1,740 unique names: eleven new
component cases, no removed or duplicated names. The focused run executed 130 cases in
5.30 seconds: 129 passed, zero failed, and only the original
`Mail.PostgresAtomicOwnershipRevisionReceiptAndRewardPacket` database opt-in skipped.
All eleven new cases and the owning TLS case passed (0.511 seconds), together with the
selected router/pipeline, admission/presence, GM-mail and merchant checks. The rebuilt
disposable memory server passed all 39 world smoke checks and all twelve mail smoke checks,
including concurrent/idempotent claim, recipient isolation and active revision adoption.
Module, source guards, exact test-union and whitespace review passed; receipts are under
`out/modular-plan/mail-integration`. These are synthetic checks, with no new original-client,
deployment, packet, schema, storage-policy or feature-status claim.

## Already separate adapters and HotKey test ownership

Friends and HotKey runtime behavior already resides in their feature/service owners;
no extra runtime wrapper was added. Fog's remaining native GM capability delegates to
its existing owning reveal implementation. The concrete leftover was five HotKey tests
and their fixture in the companion test program. They move intact to the HotKey module;
all 35 original definitions equal 30 retained vehicle definitions plus the five moved
definitions, including all 54 owning assertions, the fixture and four shared constant
values. The historical vehicle program keeps its name and remaining tests; its unused
HotKey test dependency is removed.

Actual HotKey and vehicle Release programs built. All 1,740 registered names are unchanged,
with exactly five executable owners transferred and no added, removed or duplicate names.
The entire affected scope executed 35 cases in 2.67 seconds: 33 passed, zero failed,
and the original companion and HotKey PostgreSQL opt-ins skipped. The read-only exported
companion catalog check passed. Runtime/native/bus hashes are unchanged; no world smoke
was repeated for this test-only move. Module, source/test/fixture guards and whitespace
checks passed. New preamble formatting matches; six inherited wrapping suggestions inside
the exact moved bodies were reviewed without altering those bodies. Receipts are under
`out/modular-plan/hotkey-integration`.

Reference verification also exposed a concrete reproducibility issue: the committed LF
document bytes differed from the publication digest recorded for Windows CRLF output.
The generator now writes document/catalog/Page-block outputs explicitly with LF, matching
the repository attributes. Protocol input bytes keep their existing policy. Generator
regressions and strict verification pass, and the staged document bytes must match the
verified publication digest before commit. No packet/catalog content or runtime behavior
changes because of newline normalization.

## Chat

`ChatFeature` owns the existing WorldChatService, command callback and paired front
entry/exit callbacks. `ChatSession` owns native request orchestration and the atomic
entry-report flag. Native world transport remains Party-only. Command interception stays
before the existing chat service/rate checks, and chat text is not logged. Local audience
policy reuses WorldPresence, preserves speaker echo/observer order and authenticated active
filtering, and adds no duplicate geometry or unit-identity implementation. The composition
adapter retains the same selected speaker/observer sessions until after its codec lock.

Entry still follows successful PvP registration. The handler is copied under the original
mutex before checking its entry result; late close reports one paired exit through the
same atomic exchange. Exit reads handoff identity after consuming the published flag and
copying the handler, inside the original callback catch. Existing narrow postlude and
close ordering remain unchanged. GM reply/announce ownership stays in GM.

The exact Local-audience native TLS definition moves to a separate owning Chat-native
program, retaining all seven assertions and its fixture gate; 93 retained definitions
become 92 plus one moved. Actual Chat/native/session/Party/GM/Mail/front/core test targets
and the server built. Registration grew from 1,740 to 1,747 unique names: seven new cases,
no removed or duplicated names. The expanded focused run executed 194 cases in 8.38 seconds:
193 passed, zero failed, and only the existing Mail PostgreSQL opt-in skipped. All seven
new Chat cases and the owning TLS case passed (0.540 seconds), together with old Chat,
Party, Mail/front and the selected router/admission/presence/GM/Party-TLS checks. The rebuilt
disposable memory server passed all 39 world and nine native-chat smoke checks. New-file
formatting, source/test guards, modules and whitespace passed; receipts are under
`out/modular-plan/chat-integration`. The bus, composition factory and shared native fixture
are unchanged. No packet/status/gameplay policy or original-client acceptance is promoted.

## Party

`PartyFeature` owns the single PartyWorld, catalog limits, strand, one-second tick timer
and shared delivery callback. Events receives that same PartyWorld and executor at the
original declaration position. Queued/armed timer handlers retain weak feature owners;
installed delivery retains only weak world state. Tick activation, exception boundaries
and re-arm order remain unchanged.

`PartySession` owns captured character identity, worker-owned live presence and the atomic
attachment publication. Close reads the stable identity published before attachment;
live level, unlock and channel changes do not mutate it. First attach, later refresh,
late-close detach and stale-connection protection retain their original guards. Tutorial
graduation resets the private channel before unlocking; Party loot audience and all entry,
postlude and close ordering remain unchanged.

The exact two-client Party/chat TLS definition moves with its 16 assertions, fixture gate
and timeout unchanged: 92 native definitions become 91 retained plus one owning test.
Actual Party/native, Events, Chat/native, session, GM/core targets and the server built.
All 1,747 old registered names remain, with seven new cases and no duplicate/lost names
(1,754 total). The focused run executed 221 cases in 15.05 seconds: 220 passed, zero failed,
and the original RankingStorage PostgreSQL opt-in skipped. All seven new cases and the
owning TLS case passed (0.515 seconds), together with all 57 Events cases/raid lifecycle,
Chat/Local TLS and selected route/admission/GM checks. The rebuilt disposable memory server
passed all 39 world and nine native-chat smoke checks. Source/test conservation, module,
new-file formatting and whitespace checks passed; one inherited call wrap in the exact
moved TLS test remains. Receipts are under `out/modular-plan/party-integration`. The sole bus,
composition factory and shared native fixture are unchanged. No gameplay/status/packet or
original-client acceptance change is claimed.

The remaining-work audit also assigns Housing its own explicit unit: its existing shared
hub does not own the native travel/location/pending/attachment state, entry-item payload
builder or listener expiry timer. Those responsibilities remain pending extraction.

## Guild

`GuildFeature` owns the existing guild directory, service subscription and world delivery;
`GuildSession` owns authenticated actor enrichment, atomic attachment, entry snapshots
and Guild EXP delivery. The front-created GuildService and CompanyService
instances remain shared with the world, with the same optional-service guards. Subscription
and world delivery use weak owners. Existing translation, codec and unlocked fanout scopes
remain unchanged.

Actor identity still comes from the authenticated Party identity and live Company service.
Attachment publishes stable identity before I/O close; late-close detach and once-only
entry snapshot keep their old positions. The activity cursor advances before synchronous
Guild EXP delivery, which completes before Company contribution and roster refresh.

The exact native guild-invitation TEST moves with all 17 assertions, fixture gate and
timeout unchanged: 91 native definitions become 90 retained plus one owning test. Actual
Guild/native, Company/session/front/GM, Chat/Party-native/core targets and the server built.
All 1,754 old registered names remain, with six new cases and no duplicates (1,760 total).
The focused run executed 205 cases in 19.38 seconds: 202 passed, zero failed, and the original
two CompanyServiceStorage and one GuildServiceStorage PostgreSQL opt-ins skipped. All six
new cases, owning TLS (0.516 seconds), full Company/GM/front, Chat/Party TLS and selected
route/admission/bus cases passed. The rebuilt disposable memory server passed all 39 world,
eight Guild and nine social smoke checks. Source/test conservation, modules, new runtime/
component formatting and whitespace checks passed; two inherited test-call wraps remain
unchanged. Receipts are under `out/modular-plan/guild-integration`. The sole bus, composition
factory and shared native fixture are unchanged. No gameplay/status/packet or original-client
acceptance change is claimed.

## Company

`CompanyFeature` owns the existing shared CompanyService and optional dialogue index;
`CompanySession` owns dialogue/permit orchestration and the single activity EXP cursor.
The cursor initializes at the same tutorial selection point and advances before the
synchronous Guild EXP event. Guild delivery completes before Company contribution;
the Company update packet precedes optional membership/roster refresh. A decrease resets
the cursor, and a subscriber exception does not replay an earlier award.

Dialogue guards retain authenticated account/character, tutorial and NPC queries in their
original order. Join commits before membership refresh and reply; a guild membership still
refuses Company change at the same point. Paid-permit planning and currency save remain
atomic; committed progress is adopted before the inventory update and permit response.
The reference wording is corrected to distinguish this Company-owned activity cursor from
Guild's resulting EXP delivery; no duplicate cursor exists.

All 90 retained native definitions and their file bytes are unchanged; this unit moves no
native TEST. Actual Company/Guild-native/front/GM/Chat-native/Party-native/core/session and
server targets built. All 1,760 old names remain with eight new cases and no duplicates
(1,768 total). The focused run executed 213 cases in 19.68 seconds: 210 passed, zero failed,
and the original two CompanyServiceStorage and one GuildServiceStorage PostgreSQL opt-ins
skipped. All eight new cases, full Guild/GM/front, existing Chat/Party/Guild TLS and selected
route/admission/bus cases passed. The rebuilt disposable memory server passed all 39 world,
eight Guild and nine social smoke checks. All three new C++ files match the formatter's
multiline overrides; source/test conservation, modules and whitespace checks pass. Receipts
are under `out/modular-plan/company-integration`. The sole bus, composition factory and
shared native fixture are unchanged. No gameplay/status/packet or original-client acceptance
change is claimed.

## Inventory

`InventoryFeature` owns utility definitions, capacity/weight/encumbrance rules and polish
catalogs at their original separate startup positions. `InventorySession` owns equipment,
discard, utility-bag snapshot state, encumbrance state, polish RNG and the actual inventory
stat/snapshot sinks used by Shop, GM, Market and Mail. The former character-item projection
moves once into Inventory, while Core player/observer projections remain unchanged.
Public native capacity/weight declarations remain available through their reexport.

Equipment validates/prepares stats before atomic save, then publishes equipment revision,
moves the prepared stats once into combat and immediately uses its current live view.
Encumbrance keeps its prepared move and live-view copy; polish retains the original copy
and item/stat/equipment order. Utility publishes loot progress only after a changed committed
revision, with the existing world-mobs guard in composition. Inventory snapshot observers
retain nonconsumption and ordering. Shop/GM retain recomputation-before-snapshot; Market/Mail
retain no recomputation. Existing caller-owned held and unheld codec scopes stay unchanged.
Only committed equipment revision and world-loot progress join the sole typed synchronous
bus; subscriber registration remains explicit in frame_pipeline.cpp. The later Character
extension stays private. RNG construction and post-admission initialization retain their
old positions.

All 90 native definitions equal 67 retained plus 23 exact owning moves, including all 288
assertions, fixture gates and timeouts. All 14 old encumbrance definitions and the polish
fixture/stat builder remain exact. Actual Inventory/native/Core/session/GM/Shop/Market/Mail/
Nuia/ItemUse/Gathering/Processing and server targets built. All 1,768 old names remain with
12 InventorySession and two Core event cases (1,782 unique total), no losses/duplicates,
and exactly 23 executable owners transferred.

The primary focused run executed 395 cases in 37.47 seconds: 389 passed, zero failed, six
skipped. Three generated-profile opt-ins were then enabled for only those three named cases;
all three passed. Combined unique outcomes are 392 passed, zero failed, and the three original
Mail, Market and native-Shop PostgreSQL opt-ins skipped. Both raw runs are retained separately.
All 23 moved native cases, all 14 new cases, old encumbrance, NuiFlame and GatheringSnapshot
checks passed, together with affected economy/interaction/route/admission checks. The rebuilt
disposable memory server passed world39, mail12 and market7 request smoke checks. The ten
new non-native C++ files match multiline formatter overrides; inherited exact moved-test
layout is preserved. Source/test/fixture guards, modules and whitespace passed; receipts are
under `out/modular-plan/inventory-integration`. No gameplay/status/packet/schema or new
original-client acceptance is claimed.

## Item use

`ItemUseFeature` owns optional/fatal catalog startup, immutable catalog references, selected-
subject persistence/effect binding and frame dispatch. The existing ItemUseSession remains
the player action owner. Effect capabilities install only when combat exists at creation;
callbacks resolve current combat, effects, readiness and position when called, preserving
map-rebuild behavior. Summon registration still belongs to Companion earlier in the pipeline.

The new Core grant-only helper owns the source-identical commit/cache sequence: durable
commit, current tutorial query and optional adoption, then durable actor reload and inventory
snapshot only for nonempty returned instance grants. It retains the immutable selected
Handoff subject, original exceptions, no combat recomputation and no new codec lock. This
unit wires ItemUse; Processing adopts that same helper in its later extraction. Packet
append/consumption and gameplay deadline update keep their original positions.

All 67 retained native definitions/file bytes and prior binary owners remain unchanged;
there was no dedicated opcode20077 native TEST to move. Actual ItemUse/Core/session/
Processing/Inventory-native/combat and server targets built. All 1,782 prior names remain,
with seven Core grant and five ItemUse cases (1,794 unique total), no losses/duplicates.
All 179 focused cases passed with zero skips/failures in 5.60 seconds, including 17 existing
combat-item/effect/deadline cases, all Processing/Inventory cases and the selected native,
route/admission/bus checks. The existing Processing profile opt-in was enabled. The rebuilt
disposable memory server passed world39 and market7 request smoke checks. All six new C++
files match multiline formatter overrides; source/test/fixture guards, modules and whitespace
passed. Receipts are under `out/modular-plan/item-use-integration`. The public native header,
sole bus, composition factory and shared fixture are unchanged. No gameplay/status/packet,
storage-policy or new original-client acceptance is claimed.

## Gathering

`GatheringFeature` owns the shared data/nodes, respawn strand/timer, map/sector snapshots,
request dispatch and shared delivery. The existing GatheringSession remains the player action
owner. Queued and armed callbacks retain weak feature/world owners. Sender exclusion still
precedes weak lookup/map access; selected sessions survive the codec lock through delivery,
and a packet encoding failure does not abort later recipients. Map-load snapshots append
and return false; sector payloads use the same depleted/doodad board and deterministic order.
Current combat is resolved when stamina is spent, followed by the same unlocked cache event;
Inventory tool packets remain after successful commit. Shared gathering reads durable progress,
without tutorial adoption.

Source review confirms an existing limit: world_storage.cpp::validate_item_trade accepts only
nondecreasing durability, while gathering tool wear proposes a decrease. The real-memory
regression preserves its handled refusal with unchanged progress, no spent stamina/post-commit
tool callback and no depletion broadcast. The separate successful post-commit order test uses
a narrow ProgressAccess double. The unchanged tool-free placement/doodad native TLS case
passes. This records the source/fixture boundary without repairing persistence or claiming
an observed original-client failure or a tool-free failure. Crafting reference wording now
states that limit and distinguishes source/synthetic observer delivery from client acceptance.

The owning native TEST moves intact with 29 assertions, timeouts and fixture gates: 67 native
definitions become 66 retained plus one owning definition, with exactly one executable owner
changed. Actual Gathering/Core/session/Inventory-native/Processing/Nuia and server targets
built. An initial nonexistent separate native-test target stopped before compilation; the
correct combined Gathering target then built successfully, with both logs retained. All
1,794 prior names remain plus ten new cases (1,804 unique total), no losses/duplicates.
All 203 focused cases passed with zero skips/failures in 8.28 seconds; both generated profile
opt-ins were enabled. Owning TLS passed in 0.552 seconds, together with GatheringSnapshot,
NuiFlame, affected module and selected route/admission/bus cases. The rebuilt disposable memory
server passed world39 and market7 request smoke checks. New runtime/header/component formatting,
source/test/fixture guards, modules and whitespace pass; inherited exact native layout remains.
Receipts are under `out/modular-plan/gathering-integration`. The public native header, sole bus,
composition factory and shared fixture remain unchanged. No gameplay/status/packet/schema,
storage-policy or new original-client acceptance is claimed.

## Processing

`ProcessingFeature` owns the loaded data profile, selected-subject persistence binding and
request dispatch. The existing ProcessingSession retains dialogue, pending craft and player
budget state. Missing data retains disabled behavior at the old startup position. Factory
callbacks resolve the current tutorial revision and combat stats at execution. Station and
position handling stays before the existing busy receive path, and responses replace the
packet vector as before. The Core grant-only helper is now shared with ItemUse, preserving
commit/adopt/nonempty-instance reload/snapshot order without stat recomputation, new locks
or catches.

The complete station-dialogue native TEST moves with its nine assertions, timeout and gate,
including the player wire-unit release. All 66 native definitions equal 65 retained plus one
owning move. Actual Processing/Core/session/ItemUse/Gathering/Inventory/Nuia and server targets
built. All 1,804 prior names remain plus five new cases (1,809 unique total), no losses or
duplicates; exactly one executable owner changed. All 227 focused cases passed with zero
skips/failures in 8.79 seconds, including seven shared grant cases, owning TLS (0.368 seconds),
affected modules, GatheringSnapshot/NuiFlame and selected route/admission/bus cases. Both
generated profile opt-ins were enabled. The rebuilt disposable memory server passed world39
and market7 request smoke checks. New runtime/header/component formatting, source/test/fixture
guards, modules and whitespace pass; inherited exact native layout remains. Receipts are under
`out/modular-plan/processing-integration`. The public native header, sole bus, composition
factory and shared fixture are unchanged.

The final Phase 4 ownership audit still must move Handshake's actual Processing expertise128/
craft132 entry builders and Inventory item130/money126 builders, preserving their generic
ordered encoding positions. This tracked follow-up prevents the class boundary from hiding
remaining feature policy; Phase 4 is not closed. No gameplay/status/packet/schema, storage-policy
or new original-client acceptance is claimed.


## Nuia

`NuiaFeature` owns statue/tear profiles at their original startup positions and creates
the player owner. `NuiaSession` owns lazy statue state, independent tear budget/snapshot
state, blessing adoption and pending shrine completion. Broad request routing retains
constructive lazy statue creation after its original guards; passive cold-state/deadline
queries do not construct a statue session or arm the generic timer. Blessing validates
the raw stat model before spending, then retains copied combat adoption, live-view use,
held snapshot publication and the final growth notice. Tears retain 32 requests/second
and set their snapshot bit only after successful append.

Shrine revival keeps the absent-data fallback and constructive escape pending query.
Accepted teleport precedes pending installation; completion consumes pending state before
durable work. Original catches, duplicate protection filtering, revived packet insertion
after the first teleport packet, supplies, cached stats and life publication stay ordered.

All 22 old Nuia definitions remain exact. Three owning native TLS definitions move intact
with 51 assertions and all fixture gates/timeouts: 65 native definitions equal 62 retained
plus three moves. Actual affected Release targets/server built. All 1,809 prior names
remain plus nineteen new cases (1,828 unique total), with no losses or duplicates; exactly
three executable owners changed. All 253 focused cases passed with zero skips/failures
in 10.41 seconds, including owning flame/statue/tear TLS (1.864/0.642/0.556 seconds),
Inventory/Processing/Gathering/Escape and selected route/admission/bus cases. Generated
profiles were enabled. The rebuilt disposable memory server passed world39 and market7
request smoke checks. Five new non-native C++ files match multiline formatter overrides;
inherited exact native layout remains. Source/test/fixture guards, modules and whitespace
passed; receipts are under `out/modular-plan/nuia-integration`. The public native header,
sole bus, composition factory and shared fixture remain unchanged. No gameplay/status/
packet/schema, storage-policy or new original-client acceptance change is claimed.


## Character

`CharacterFeature` owns stat/progression/loadout startup, entry reconciliation and graduation.
`CharacterSession` owns actual progression and GM edit algorithms; `CharacterBootstrap` owns
selected character, stats, life, GM and perk cache plus Base125/Perk135 builders. Handshake
retains generic entry ordering. Validation precedes commit, current tutorial revision is
resolved at execution, plain progress saves stay distinct from atomic item trades, and
combat stat copies precede the same locked cache/inventory effects. Level changes retain
unlocked Party update before locked cache/refill and observer effects.

Two borrowed synchronous topics extend the sole bus for progression cache and Party level
effects. Publication copies neither build nor subscriber lists; the existing owned Handshake
cache still copies the build. Graduation retains independently caught quest, item and final
level stages, so an earlier stage failure does not suppress the old final level attempt.
The bounded quest-completion helper lives in Quests, actual shared item-bundle values live
in Shop, and GM retains compatible aliases plus its nine-kind progression edit conversion.
Character has no reverse GM/Quests dependency. QuestsSession remains its later work unit.

Sixteen complete graduation definitions move to Character with 38 assertions, both original
PostgreSQL gates and the two real-catalog memory cases. All 62 retained native definitions/
file bytes remain unchanged; no native move is claimed. The actual 27-target affected
consumer/server Release build passed (104 commands, no clean/full-phase build). All 1,828
prior names remain plus nineteen new cases (1,847 unique total), no losses/duplicates; exactly
sixteen graduation binary owners change. Of 220 focused cases, 216 passed, four original
opt-ins skipped and zero failed in 19.64 seconds. Skips are the two graduation PostgreSQL
cases, one progression PostgreSQL case and the private-capture stat comparison. All new
cases, fourteen non-PG graduation cases and sixteen selected native cases pass. The rebuilt
disposable memory server passed world39 and market7 request smoke checks.

All 36 reviewed paths/deletion and sole Character bus/factory changes match the frozen
candidate. Eighteen new non-legacy-test C++ files match multiline formatter overrides;
exact inherited graduation layout remains. Module/whitespace/source guards pass; no
unexpected Cursor source edit or competing main build was observed. Receipts are under
`out/modular-plan/character-integration`. Housing and the Combat bus remain private.
No gameplay/status/packet/schema, storage-policy or new original-client acceptance change
is claimed. Final Inventory/Processing bootstrap ownership closure remains required.


## Quests

`QuestsFeature` owns the late optional catalog loader; missing data disables and malformed
present data retains its fatal startup position. `QuestsSession` owns the quest book/dialogue,
request and monster reward algorithms plus the connection snapshot guard. Request guards,
ordered appends, sequenced conflict response and revision exhaustion remain unchanged.
The time-sync snapshot bit is set only after successful append and stays set across book
rebuilds. Ordinary successful requests retain only their old conditional world-loot effect;
monster rewards preserve authoritative reads, no-book EXP fallback, durable commit, cache,
Character level effects and packet append order. Failed storage emits no effects or packets.
GM acceptance/completion and entry graduation use the same guarded book/catalog capabilities.

Actual affected Release targets/server built (18-command plan, no clean/schema work). All
1,847 prior names remain plus ten cases (1,857 unique total), no losses/duplicates or changed
binary owners. All 62 native definitions/file bytes remain unchanged; no dedicated native
move is invented. Of 187 focused cases, 185 passed, two original graduation PostgreSQL gates
skipped and zero failed in 23.46 seconds. All 33 Quests cases (23 old plus ten new), 36 routes,
19 admission, bus, Character/graduation and nine selected native cases passed. Tutorial quest
objectives TLS passed in 2.71 seconds. Generated profiles were enabled. The rebuilt disposable
memory server passed world39 and market7 request smoke checks. All six guarded paths match;
four new C++ files match multiline formatter overrides. Module/whitespace/source guards pass;
no unexpected Cursor source edit was observed. Receipts are under `out/modular-plan/quests-integration`.
Public header, sole bus, composition factory and common fixture remain unchanged. Housing and
Combat bus are later units. No gameplay/status/packet/schema, storage-policy or new original-
client acceptance change is claimed.


## Housing

`HousingFeature` owns the existing Hub and minute expiry timer. Its callback acquires the
same weak listener control block before accessing members; posted worker work takes a
second weak lease. Rearm and post-stop behavior are preserved. Normal stop cancels; listener
destruction resets the optional timer without throwing, restoring its former member-timer
cancellation even when sessions retain shared Feature data. No shutdown drain is added.

`HousingSession` owns travel/origin/pending/location state and atomic presence/attachment.
The containing-session task and SessionPort retain the actual connection lifetime. Access
construction stays before the task catch, and tutorial adoption keeps its narrow logic_error
catch. Public origin is captured before raid release; pending Housing entry follows accepted
movement. Housing arrival stays first while generic pending_exit continues serving Events,
GM and natural travel. `HousingBootstrap` owns the non-tutorial cache and actual 131 builder,
sharing its ordered/filtering algorithm with live storage; its empty guard and slot before
voucher 142 are unchanged.

Actual 22-target affected consumer/server Release build passed. All 1,857 prior names remain
plus 15 (1,872 unique), no losses/duplicates. Exactly one native binary owner changes:62 old
definitions equal 61 retained plus the intact Housing TLS test with 30 assertions and all gates.
Of 303 focused cases,300 passed,three original PostgreSQL opt-ins skipped and zero failed in
24.78 seconds. Skips are Ranking best-entry persistence and the two Housing atomic/restart
cases. All 15 new, owning HousingTLS(0.847 seconds), EventsraidTLS(13.549 seconds), six GM TLS,
36 routes/19 admission/front/map/bus checks pass. The rebuilt disposable memory server passed
world 39 and market 7 request smoke checks. All 13 frozen outputs match; six new C++ files match
multiline formatter overrides while exact inherited native TEST layout remains. Modules,
whitespace and source guards pass; no unexpected Cursor source edit was observed. Receipts
are under `out/modular-plan/housing-integration`. Bus/factory/common fixture remain unchanged.
No gameplay/status/packet/schema, storage-policy or new original-client acceptance change
is claimed.

The 28-module residual ownership audit is complete in private preparation. Final closure
still must move Inventory/Processing entry policy and the shared money helper, Combat entry
presentation, Tutorial class allowlist, Party/Guild label replication and Chat notice bodies.
These have explicit owners and guarded candidates; their private status does not close Phase 4.


## Companion

`CompanionFeature` owns catalog/world startup and factory plus GM/refusal, request and
entry/leave orchestration. Existing CompanionSession remains the real player state owner;
no extra player shell or reverse Companion-to-Trade dependency is introduced. Selected
Handoff persistence, absent-data/supported-ID guards, GM/player-car budgets, epoch reads
and replies remain ordered. Joint dispatch stays commands, Trade manager dialogue,
Companion requests, Trade requests. Get-off precedes wagon settlement. The worker close
retains the connection and preserves detach delivery before wagon persistence and the
same catches; immutable Shop definitions are the same startup instance.

The actual six-target affected Release build passed. All 1,872 prior names remain plus
five (1,877 unique), no losses or duplicates. Exactly two native binary owners change:
61 definitions equal 59 retained plus intact horse/wagon TLS definitions with 42 assertions
and original gates. All 30 old vehicle component definitions retain exact bodies and their
existing binary owner in this unit. Of 191 focused cases, 190 passed, one original
CompanionStorage PostgreSQL case skipped and zero failed in 18.09 seconds. Horse/wagon
TLS passed in 2.605/2.666 seconds with the real catalog/placements. All five new cases,
36 routes, 19 admission, GM/map/presence checks and rebuilt-server world 39/market 7 smoke
checks passed. All ten frozen outputs match. Five new C++ files match multiline formatting;
exact inherited native TEST layout remains. Modules, whitespace and source guards pass.
Receipts are under `out/modular-plan/companion-integration`. Public header, sole bus,
factory and original common fixture remain unchanged. Trade definitions/bootstrap and its
ten component-owner moves remain the next unit; its known native fixture failure was not
selected here. No gameplay/status/packet/schema, storage-policy or new original-client
acceptance change is claimed.


## Trade

`TradeFeature` owns manager/catalog startup, factory and dialogue/request policy. Existing
TradeSession remains the mutable player run owner. Manager loading stays at its declaration
position and the placed catalog at its later constructor-body position; Shop receives the
same catalog. Selected-Handoff and absent-data guards, epoch reads and joint Companion/Trade
routing retain their ordering. `TradeBootstrap` owns the actual voucher cache and packet142
builder: ordered unredeemed-only filtering includes expired unredeemed records, suppresses
empty snapshots, clears for tutorial entry and keeps its slot after Housing131.

The first actual consumer build stopped because the public Handshake header included the
full TradeFeature header, exposing Core ProgressAccess alongside housing::ProgressAccess
in an unchanged owning Housing TEST. The approved three-path repair moved the exact
bootstrap declaration/comment to a narrow persistence/protocol header and reexported it
from TradeFeature. It changed neither implementation nor TEST bodies. The repaired
26-target affected-consumer/server build passed, including that unchanged Housing TU;
Housing TLS passed in 0.865 seconds. Original failure and repair logs remain separate.

All 1,877 prior names remain plus three (1,880 unique), no losses/duplicates. Exactly eleven
old binary owners change: ten Trade component definitions move from the vehicle target and
one intact Trade TLS definition moves from Session. Thirty old component definitions equal
twenty Companion-retained plus ten Trade-owned; 59 old native definitions equal 58 retained
plus one move with 25 assertions. All bodies, fixture gates and timeouts are exact.

The primary 197-case run passed 196, skipped one original CompanionStorage PostgreSQL gate
and failed none in 18.80 seconds. The separate fixture-enabled owning Trade TLS case failed
in 2.613 seconds with the exact established exception, `Placement file has no unit for the
requested NPC`, matching the controlled pre-pipeline 3.37-second and prior pipeline
3.51-second receipts. Combined unique outcomes across 198 are 196 passes, one original skip
and one established baseline fixture failure; this is not an all-green expanded run.
No policy, assertion or fixture gate was changed to hide it. All three new cases, all thirty
old component cases, 36 routes/19 admission, GM/map/bootstrap checks and rebuilt-server
world39/market7 smoke checks passed. Eleven final paths match frozen payloads plus the
approved repair. Four new C++ files match multiline formatting; exact inherited TEST layout
remains. Modules, whitespace and source guards pass; bus/factory/common fixtures remain
unchanged. Receipts and repair manifest are under `out/modular-plan/trade-integration`.
No gameplay/status/packet/schema, storage-policy or new original-client acceptance change
is claimed.


## Shared Combat

CombatFeature owns the actual combat data, authored shared-map populations and definitions,
weak private-instance cache/mutex, fall/movement/revival rules and PvP registry. The three
startup stages remain at their original constructor-body positions relative to Inventory,
other catalog owners and later Trade/Quests/Nuia loading, preserving optional versus fatal
inputs. Raid admission resolves before the instance-cache lock. Solo instances retain the
high-bit character key space, raid channels retain their existing key, expired weak entries
are pruned at the same point, and live populations remain shared by the same sessions.
Native composition supplies the existing result-bearing raid admission capability and
retains all player behavior for the subsequent independent player extraction.

The actual six-target Release build passes. All 1,880 prior names remain plus ten new
Shared/startup cases (1,890 unique), with no losses, duplicates or binary-owner transfers.
All 58 native TEST definitions, the public Native header, repaired Trade headers, GM ports,
Core bus, composition factory and common fixture are unchanged. All six applied paths
match the immutable shared payload; the four new handwritten C++ files match formatting.
Module, whitespace and source guards pass.

The focused 428-case run passed 419, failed none and retained nine original opt-in skips
in 40.36 seconds. Eight skips require version-specific archived profiles not supplied in
this run; one is RankingStorage PostgreSQL. Current generated tutorial and world-mob
profiles were enabled. All ten new cases passed, as did real shared population TLS
(3.100 seconds), private instance TLS (2.817 seconds), Gathering population preservation
(3.011 seconds), raid TLS (13.550 seconds), GM checks, 36 routes and 19 admission cases.
The rebuilt production server passed all 39 world and seven market smoke checks.
This selected gate does not claim a new full-suite run or resolution of the seven
controlled pre-pipeline fixture failures. Raw receipts are under
`out/modular-plan/combat-shared-integration`; the player-session candidate has a separate
independent source/lifetime review and remains unapplied at this checkpoint. No gameplay,
packet/status, schema, storage-policy or new original-client acceptance change is claimed.


## Player Combat

CombatPlayerSession owns the actual CombatSession, fall tracker and movement-vitals state,
durable death/revival, player request/tick policy and PvP lifecycle. Native retains mixed
timer, worker/shared-session and codec orchestration. Its named capabilities resolve the
current borrowed engine immediately; no other feature retains that mutable model. World
population attachment still precedes the loot progress read, including the missing-data
case. Private tutorial completion and generic map departure keep their distinct model/loot/
observer teardown sequences. Posted PvP work retains the containing session and completion,
with owner encode/post before observers and completion, including the old catch behavior.

The ordinary snapshot subscriber queries the current engine directly, retains the old owned
Handshake stats copy and cannot recursively republish. The separate borrowed synchronous
PlayerLifeCommitted event occurs only after the original successful durable death/revival
commits and preserves the caller's codec scope. Twenty-one typed GM engine operations and
a presence query replace the raw mutable-model port; command parsing, authorization, budget,
ordering and all 24 original GM component TEST definitions remain intact. Five bootstrap
packet factories preserve the original eight gauge/attribute/trait/dead-presentation
construction, encode and append positions, including StartPlaying and saved-dead reconnect.

The corrected actual 19-target Release build passed 68 commands, including the production
server. The first command stopped before compilation because its Market target name was
incorrect; the raw command error and corrected build logs remain separate, with no source
repair. All 1,890 old names remain plus 16 new cases (1,906 unique), no losses or duplicates.
Exactly 13 original native definitions move to Combat:58 equal 45 retained plus 13 complete
TESTs with 114 direct assertions and original gates/timeouts. The public Native header,
Trade repair and shared Combat payloads remain unchanged. All 22 promoted paths match the
frozen candidate. Eight new non-native handwritten C++ files match formatting; old native
TEST/DeathFixture layout remains exact. Module, whitespace and source guards pass.

Primary 550:541 passed, none failed and nine original opt-in cases skipped in 75.19 seconds.
The skips remain eight unavailable archived-profile comparisons and one Ranking PostgreSQL
gate; current generated profiles are enabled. All 16 new cases, all 24 original GM definitions,
GM program checks, nine safe owning Combat TLS cases, current-model cross-feature checks,
36 routes and 19 admission cases pass. Rebuilt-server world39/market7 smokes pass.

The separately enabled four moved life cases all reproduce their controlled pre-pipeline
failure families under the same three fixture hashes, parallelism 2 and unchanged properties:

| Case | Actual result | Current seconds | Controlled baseline seconds |
| --- | --- | ---: | ---: |
| InPlaceReviveIsDurableRepeatableAndIgnoredForALivingOrRepeatRequest | socket-read timeout |45.191|46.01|
| FallDamageReachesSocketAndLethalLandingPersistsThroughReconnect | socket-read timeout |7.673|8.40|
| SharedMapFallDamagePersistsDeathAndSupportsRevival | same two assertions, missing revival/dead state |2.727|3.42|
| SharedMapDrowningTimerPersistsDeathAndRevivalClearsTheDive | same dead-state assertion |4.924|5.69|

Raw CTest returned 8 for that four-case group; its classification wrapper returning 0 means
the expected comparison completed, not that those tests passed. Combined 554 unique outcomes
are 541 passes, nine original skips and four established failures. No assertion, fixture,
gameplay or catch policy was changed to hide them, and no resolution is claimed. The other
three controlled pre-pipeline failures remain in the earlier baseline ledger. Receipts,
13-owner proof and per-case signatures are under `out/modular-plan/combat-player-integration`;
the independent player source/lifetime review is under `out/modular-plan/combat-player-review`.
This gate is not a new full-suite run or new original-client acceptance.


## Loot

LootFeature owns immutable definitions, the shared private-instance claim board and the
actual Party loot audience. LootPlayerSession owns the LootSession and shared-world progress
cache. Private tutorial persistence binds the passed Handoff before the containing session
installs its identity. The Party callback resolves the current channel, while delivery keeps
the same strong WorldState and connection capture. The private observer retains its borrowed
domain pointer; the shared observer resolves the current owned domain as before.

Shared attachment still reads progress before the missing-data guard. A successful item trade
adopts committed progress and refreshes the current prepared snapshot before returning replies,
without a new stat recomputation, codec scope, catch or scheduling layer. Reset preserves the
same dormant progress cache, and tutorial completion versus generic map departure retain their
distinct model/loot/observer ordering. Guard and clock-read behavior is unchanged.

The actual eight-target Release build and production server link pass. All 1,906 old names
remain plus seven new cases (1,913 unique), with no losses, duplicates or binary-owner
transfers. All 45 native TEST definitions remain in place; mixed monster/reward TLS cases
retain Combat ownership. No unrelated case is moved to manufacture Loot test ownership.
All five applied paths match the frozen candidate. The public Native header, bus/factory,
common fixture and other post-player-Combat paths remain unchanged. Three new handwritten
C++ files match formatting; module, whitespace and source guards pass.

All 257 focused checks pass, with zero failures or skips, in 26.06 seconds. These include
all 89 old Loot cases and seven new cases, generated Loot input enabled before the run,
real mixed population/reward TLS, PvP/swimming, Party/Inventory/Tutorial TLS, 36 routes and
19 admission cases. The rebuilt production server passes all 39 world and seven market
smoke checks. The unchanged known life failures were not needlessly rerun for this narrow
step; their separate player-Combat comparison remains recorded. This is selected validation,
not a new full-suite run or original-client acceptance. Raw receipts are under
`out/modular-plan/loot-integration`; the independent source/lifetime and exact native TEST
partition review is under the private remaining-feature candidate's `root-review/`. No
gameplay, packet/status, schema or storage-policy change is claimed.


## Tutorial

TutorialFeature owns the immutable base profile and real supported-class predicate.
TutorialPlayerSession owns the actual TutorialSession/progress, separate snapshot/sector
flags, instance rewards, frame tail and map exit. Bootstrap supplies preloaded progress
and explicit Handoff-bound storage callbacks before the containing session publishes its
current identity. Reward commit keeps the original free-slot and epoch assignment, atomic
item trade, success-only inventory refresh and returned-row ordering.

The frame tail preserves snapshot-versus-sector assignment, before/after Combat advances,
conflicting skill refusal, revival/Combat/Loot/Tutorial request order, clock reads and the
level/encumbrance/death/activity sequence. Tutorial completion releases combat before its
borrowed private loot domain, selects the optional followup, notifies Party before resetting
Tutorial, then performs mail/notice/service transitions before position persistence. Native
retains combined-deadline sampling, codec scope and encoded frame/handshake/map presentation.
The supported-class predicate moves without broadening the original four-class allowlist.

All nine applied paths match the immutable candidate. The actual 12-target Release build
linked the production server and affected tests; the original terminal tool output was
truncated, so an immediate identical-target verification returned zero with no work pending.
All 1,913 old names remain plus 12 new cases (1,925 unique), no losses or duplicates. Exactly
four native definitions change binary owner: 45 equal 41 retained plus four complete moves
with 105 direct assertions, original gates and timeouts. The public Native header, bus,
composition factory, common fixture and previously frozen Loot payloads remain unchanged.
New production/component code matches formatting; inherited native TEST wrapping/comments
remain exact. Module, whitespace and source guards pass.

The focused 354-case run passed 351, failed none and skipped three original PostgreSQL
gates in 39.30 seconds: two real-catalog graduation cases and one TutorialStorage ownership/
atomicity/revision case. All 12 new cases, the complete selected Tutorial domain, all four
fixture-enabled owning TLS cases, 36 routes and 19 admission cases pass. None of the four
owning cases overlaps the controlled seven baseline failures. The rebuilt production server
passes all 39 world and seven market smoke checks. This is selected validation, not a new
full-suite run, PostgreSQL execution or original-client acceptance. Raw receipts and exact
owner transfers are under `out/modular-plan/tutorial-integration`; the independent source/
lifetime and original native partition review is under the remaining-feature candidate's
`root-review/`. No gameplay, packet/status, schema or storage-policy change is claimed.


## NPC

NpcFeature owns immutable population, the original shared epoch and optional static NPC
index. NpcPlayerSession owns actual map entry, announcement, sight, patrol and interaction
resolution. Composition/Core retain the bounded registry, authoritative map/instance views
and generic sector-window rearming. Same-map assignments remain inert; changed maps clear
registry, patrol and sight in the original order. Entry collisions remain fatal and each
entry announces once. Targeted patrol synchronization retains the original request exclusions.
Sight refresh and announcements precede visible patrol movement, without new scheduling,
audience selection or map authority. Optional static data loads at its original startup slot.

The actual ten-target Release build and production server link pass. All 1,925 old names
remain plus eight new cases (1,933 unique), no losses or duplicates. Exactly 12 binary owners
change: 41 original native definitions equal 29 retained plus 12 exact NPC moves with all
78 direct assertions, fixture gates and timeouts preserved. PlayerWireUnitId socket coverage
remains with Session. The shared map-entry fixture moves once into the application-test-local
`native_map_entry_test_support.hpp`, used by both the NPC owner and retained mixed map case;
the accepted common base fixture is unchanged. All eight outputs match the frozen candidate.
The public Native header, bus/factory and earlier feature payloads remain unchanged.

All 264 primary cases pass, with zero failures or skips, in 39.33 seconds: eight new cases,
the full selected NPC domain, 11 safe owning TLS cases, selected cross-feature TLS, 36 routes
and 19 admission cases. The separate fixture-enabled owning
OverworldAndTutorialSessionsStayIndependentOnOneServer returns raw CTest 8 in 6.206 seconds,
matching the controlled pre-pipeline 7.0-second and earlier 6.8-second failure family. Both
saved baseline and current output contain 20 assertion occurrences at two unique sites:
19 reply-opcode mismatches (177 versus 2), one opened-opcode mismatch (177 versus 163), then
the same missing shop_id exception. The first parser counted only cpp sites; its initial
result is retained and the saved-only corrected comparison includes the moved hpp site and
exact expectation/frequency anchors. No test rerun or source/assertion/fixture repair occurred.
The three controlled fixture hashes and test properties remain unchanged. Combined 265 unique
outcomes are 264 passes and one established failure, zero skips; this is not all-green.

Rebuilt-server world39/market7 smoke checks pass. New runtime/component code matches formatting.
The moved native TEST/helper bodies retain their inherited layout and exact evidence. The
short constructor and worker lambda at shared fixture line 51 are a recorded required finishing
item for Root's guarded Phase 4 boundary cleanup; this is not a final style exemption. Module,
whitespace and source guards pass. Raw failure/parser/style previews and exact owner receipts
are under `out/modular-plan/npc-integration`. This selected gate is not a full-suite run, a fix
for the seven controlled failures, or new original-client acceptance. Phase 4 still requires
both ownership closures, final style/graph review and its complete boundary validation.


## Inventory and Processing bootstrap closure

InventoryBootstrap owns the real non-tutorial optional gold, token and ordered stack-map
cache and the item 130/money 126 builders. It reads the current CharacterBootstrap character
at the original item-construction point, without duplicating that character. Configuration
moves only stacks from WorldProgress; stats/dead preparation and Housing/Trade extraction
retain their order. Engaged zero gold still produces the same snapshot, and ordered
zero-quantity stack tombstones remain included. Nonzero-only token inclusion remains the
existing bounded candidate rather than a stronger original-client claim.

The world_money_values_json body moves once into Inventory with its original public
signature and a Tutorial header reexport, avoiding an Inventory-to-Tutorial dependency.
Processing's existing real Feature gains the expertise 128 and empty learned-crafts 132
packet factories; its old expertise JSON helper/API remains unchanged. Handshake constructs
items before expertise encoding, then keeps 128/130/132, perks and 126 at their old encode/
append positions. The temporary expertise/craft scopes end at the original full-expression
points, preserving partial results and exception order. No vector is prebuilt and no new
lock, catch, stat recomputation or character/progress authority is introduced.

The actual ALL Release build passes all 158 commands at three jobs, including every
application and test consumer of the changed public header and moved static-library symbol.
All 1,933 old names remain plus four new cases (1,937 unique), no losses, duplicates or
binary-owner changes. All 153 pre-existing test source files remain byte-exact, including
the 29 retained native definitions; no test is relocated for this closure. All 12 applied
paths match the frozen candidate. Four new C++ owner/test files match formatting, and
reviewed include/blank-run finishing corrections are preserved. The shared MapEntry style
item, bus/factory and narrow Trade headers remain outside this change. Module, whitespace
and source guards pass.

Focused 415: 408 passed, zero failed and seven old opt-in cases skipped in 26.00 seconds.
Six skips are PostgreSQL gates: graduation 2, progression 1, Housing 2 and TutorialStorage 1.
The other is CharacterStats' existing private-capture comparison; that input was not
supplied. Current catalog/profiles are enabled. All four new cases and selected real entry,
money/token/tombstone, Inventory/Processing/Tutorial/Character/Housing/Trade, header-consumer,
36 route and 19 admission checks pass. The rebuilt server passes all 39 world and seven market
smoke checks. No unrelated controlled-seven rerun was added. Receipts, preserved 153-file
proof and actual ALL build identities are under `out/modular-plan/bootstrap-integration`;
independent nine-check semantic review and guarded preparation remain in the private
bootstrap candidate. This is selected validation, not a full-suite run, PostgreSQL/private
capture execution or new original-client acceptance. Both the replication closure and
complete Phase 4 boundary gates remain required.


## Party/Guild replication and Chat notice closure

The existing PartyFeature and GuildFeature own the actual addressed-packet and membership
recipient-expansion loops. Their three-operation contexts queue one packet, obtain current
observers and queue one shared observer message. They receive no application/session object
or service locator. Packets precede unit changes; each subject receives its label before
its current observer query, preserving observer order and duplicates. Guild resolves each
character at the original loop position and skips offline subjects. Its Native availability
guard remains before the codec lock; the owner's disabled-directory check is inert on that
path and does not introduce a new runtime policy.

Native retains the same instrumented lock, local-frame append and generic weak lookup.
The stack adapter holds borrowed references and adds no callback conversion, allocation or
strong recipient owner. Each addressed encode keeps its original catch. Shared messages
still encode once and allocate one immutable byte buffer inside the catch enclosing their
full observer queue loop; failure preserves the same partial vector. Observer-query failures
remain outside that catch. The generic queue helpers are byte-identical to the frozen
bootstrap input; all strong recipients survive through each post-unlock send.

Chat owns the opcode 3 text/type notice builder, style mapping and public enum declaration.
The enum namespace and values are unchanged and the public Native header reexports it.
JSON construction remains before locking, with encoding and live in-world selection inside
the same mutex and retained sends afterward. The complete audience/lock/send body is exact
after only two named builder substitutions. Captured CMS keys remain distinct from the
unobserved original-client display of free-text operator notices.

Actual ALL Release at three jobs passes, linking every affected test/application consumer.
All 1,937 prior names remain plus six cases (1,943 unique), no losses, duplicates or binary
owner transfers. All 155 existing test source files remain byte-exact. All 17 applied paths
match the frozen candidate and independent bridge review. Ten new C++ files match formatting;
module, whitespace and source guards pass. The six bootstrap owner payloads, shared MapEntry
fixture, bus/factory and earlier feature payloads remain unchanged. The shared membership
test helper adds no new upper dependency to the existing testing target.

The focused 320-case run passes 317, fails none and skips three original PostgreSQL gates
in 24.29 seconds: two CompanyStorage cases and one GuildStorage case. All six new cases,
selected social/GM/notice/history/presence/current-model TLS, 36 route and 19 admission checks
pass. Rebuilt-server world39/market7 smoke checks pass. No unnecessary controlled-seven
rerun was added. Receipts and 155-file proof are under `out/modular-plan/replication-integration`;
the independent actual-loop/bridge/lifetime review is under `out/modular-plan/replication-peer-review`.
This selected gate is not full-suite, PostgreSQL or new original-client acceptance evidence.
Root's final shared-fixture style delta and complete Phase 4 boundary gates/measurements are
still required before Phase 4 closes.


## Phase 4 boundary verification and remaining migration closure

Actual ALL Release, the full default suite and all available generated-current cases
pass for the major feature/bootstrap/replication source at `83f89a9`. The shared MapEntry
constructor/lambda cleanup (`6ce3766a` to `9f1d0685`) preserves executable tokens and
finishes its previously pending style step. All 28 feature modules and actual bootstrap,
startup and teardown owners were audited. The remaining Core, neutral event/lifetime,
named capability/WorldState and final Expression/GM policy work is explicit in the Phase 5
plan. Its focused GM-interface unit addresses Cursor's remaining 91-method cohesion
finding. This boundary does not claim the whole migration is complete.

All 1943 registrations are unique: the 1645 prior names remain plus 298 new cases.
All 116 original native and 16 graduation definitions remain conserved, including 118
legitimate binary-owner changes. Full default CTest passes 1824, fails none and skips
119 opt-ins in 32.82s at parallelism 8. The reviewed current-fixture cohort passes 64/64
with no skips in 76.10s at parallelism 2. The automatic 66-case list remains preserved;
two hidden PostgreSQL graduation requirements are explicitly deferred.

The separate controlled seven all fail, skip none and retain raw CTest exit 8 (69.62s).
A saved comparison verifies the same assertion counts, expectation frequencies, values,
exceptions and complete timeout tails. Its only aliases are three exact helper-namespace
spellings and two repository-query expressions, backed by an identical complete
WorldTestClient class, repository implementation and old direct forwarder. Seven positive
and 31 changed-value/count/frequency/cause/tail/query/extra-exception controls pass. The
frozen b874 parser, baseline signatures and failed strict output remain untouched. No
runtime rerun, baseline-source swap or assertion/gameplay repair was used for this annex.
The three controlled fixture hashes remain unchanged.

World, Market and Mail smoke checks (39, 7 and 12) pass inside full CTest without duplicate
runs. All default skips were source-classified. The bounded encode-once opt-in passes,
then its original key absence is restored. Account-session behavior also passes inside
the existing distribution smoke. The remaining 46 optional limits cover PostgreSQL,
historical/capture profiles and documented standalone launcher backend fixtures; they
are not claimed as executed. Semantic TCP/offline replay and the independent comparison
pass for 17055 records, retaining 4185 JSON formatting differences and the documented
incomplete 753-byte tail. Raw packet/session data stays private. The reference generator's
11 unit checks pass.

The initial full attempt was invalid because the ignored harness turned absent opt-ins
into present empty variables (1824 passes, 104 failures, 15 skips). Its raw output is
preserved. The corrected runner removes child keys and restores their original presence;
an actual child probe verifies all 25 controlled keys absent. Its initial database audit
finds 28 refused loopback connections and two source-proven memory fallbacks: no
PostgreSQL case passed or established a successful connection. Passed-output capture was
also corrected to retain complete smoke JSON. Source, assertions and properties stayed
unchanged.

Canonical `tools/measure_build.ps1` completes clean/source/header/shared-header scenarios
in 283.9/15.7/22.7/211.4s using the existing build/dependency cache. Both old Ninja logs
are preserved before its force-move; all 599 source contents remain identical and all
three touched timestamps restore. The actual cached Ninja 1.13.2 uses 26 default jobs
on 24 logical CPUs without a preset/CMake job override. Private compilers were paused;
existing applications and services remained running. Observed CPU load is 54% before
and 35% after. Hardware-control and idle ReSharper identities are recorded separately.
The source edit meets the under-one-minute goal; this loaded single run does not establish
a matched speedup/regression or the 3x clean-build target.

The canonical child exits 0 and postchecks pass. Its outer wrapper exits 1 from a stale
LASTEXITCODE left by `ninja --help` (which returns 1); a postmeasurement annex verifies
the actual cached tool and preserves both statuses. No timing was repeated. Two earlier
preflight-only stops occurred before any clean, log move, source touch or document append.
Module and whitespace checks pass. Style claims cover the reviewed constructor/lambda
change; inherited whole-fixture wrapping differences remain disclosed.

Receipts are under `out/modular-plan/final-phase4-gate-83f89a9-corrected` and
`out/modular-plan/final-phase4-canonical-timing-83f89a9-20261029T112814-c974465e`.
No deployment, schema change, new original-client trial or capacity claim follows.
