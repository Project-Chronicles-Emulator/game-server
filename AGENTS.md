# Code quality rules

Write code for the next human who must read, debug, and maintain it. Readability,
correctness, and maintainability take priority over saving lines or tokens.
Apply these rules to new and modified handwritten code.

## Published changelogs

- When publishing changelogs or release notes for players, list what players can see
  or do differently in the game, in plain player language. Write each item as a
  feature or fix from the player's point of view (for example "Guild masters can now
  disband their guild"), not as implementation (for example "The guild service now
  calls X, which sends an HTTP request").
- Omit internal implementation details, protocol and infrastructure work, tooling,
  refactors, and tests unless they produce a change players notice; then describe only
  that visible result. Leave out work that is not in the released build or that
  players cannot reach. If a release has no player-visible changes, say so instead of
  filling the changelog with internal work.
- This applies to published player-facing changelogs across all channels. Technical
  reports, developer documentation, and commit messages may retain internal details.
- Public release posts contain only player-facing text. Never publish test counts, build,
  commit, review, backup, rollback, migration, deployment or capacity details, or
  release-check files. Use the standard patch-notes and server-status layouts in
  `.cursor/rules/player-changelogs.mdc`. The download announcement starts with
  `@everyone`; no other release post mass-mentions.

## Mandatory protocol and feature reference

- Before feature, protocol, persistence, launcher integration, or validation work, read
  `docs/protocol-feature-reference.md` and the relevant feature's owning source/evidence.
  `docs/protocol-reference-maintenance.md` identifies the canonical inputs and published Page.
- Update `protocol/feature-coverage.json`, affected packet overrides, remaining limits,
  and validation evidence in the same change as the feature. Keep implemented, partial,
  missing, schema recovery, official capture evidence, synthetic tests, and observed
  local-client behavior separate. Decoding, ignoring, or rejecting a message does not
  implement its feature. For pure refactors, record the preserved contract and checks.
- Review changed sources before refreshing their recorded hashes. Run
  `python tools/build_protocol_reference.py`, synchronize the published Page, and finish
  with `python tools/build_protocol_reference.py --check`. Follow the maintenance guide
  for deliberate review refresh and guarded Page edits. Never hand-edit generated outputs.
- If Page access is unavailable, retain the local update and explicitly report the stale
  Page; do not claim both copies are current. Never publish raw captures, player/session
  values, chat text, credentials, or access-bearing URLs in either copy.
- Before publishing a launcher version, follow `docs/launcher-auto-update.md`; preserve the
  independent game channel and verify the signed launcher update and player data end to end.

## Formatting and whitespace

- Never write minified or compressed source code, even for prototypes or small fixes.
- Use one statement per line. Do not combine declarations, assignments, calls,
  error checks, or returns on the same line. Normal `for` loop headers are fine.
- Put function bodies on multiple lines. Separate function definitions and
  independent declarations with a blank line.
- Inside functions, separate logical steps with blank lines: validation, setup,
  processing, and results. Keep closely related statements together; do not add
  a blank line after every statement.
- Use consistent indentation. Follow the project's formatter configuration;
  if none exists, use four spaces in C++, C#, and Python.
- Use spaces around binary operators and after commas. Write `if (condition)`
  and `for (...)`, with normal spacing inside expressions.
- In C++ and C#, always use braces for conditional and loop bodies, including
  single statements. Put statements inside the braces on separate lines.
- Wrap long signatures, calls, conditions, and initializers at logical boundaries.
  Follow the configured line limit; otherwise aim for 100 columns. Do not split
  an indivisible string or URL merely to satisfy that target.
- Expand lambdas and nested control flow when they contain branches or multiple
  operations. Avoid nested ternaries, chained side effects, and clever expressions
  that hide execution order.
- Follow established readable naming and brace placement. Existing compressed
  code is not a style precedent; do not reproduce it.

## Names, comments, and intent

- Use descriptive names that explain purpose and units. Avoid names such as `b`,
  `n`, `p`, or `tmp` for meaningful state. Short conventional loop indices are fine.
- Name non-obvious constants instead of scattering magic numbers and strings.
- Add comments when they explain why a decision exists, an invariant, a protocol
  constraint, ownership, units, or a non-obvious workaround.
- Do not narrate obvious statements or compensate for unclear code with comments.
  Simplify the code first. Keep comments accurate when behavior changes.

## Responsibilities and reuse

- Give each function one coherent responsibility. Separate unrelated concerns
  such as file access, parsing, validation, business logic, and presentation.
- Keep orchestration readable from top to bottom. Extract a named helper when
  it clarifies a distinct operation, repeated logic, or deep nesting.
- Search for existing utilities and established patterns before adding a new
  helper, class, dependency, or abstraction. Reuse or extend suitable code.
- Keep shared behavior in one appropriate place. Do not copy and paste parsing,
  validation, conversion, or error handling across callers.
- Do not force unrelated behavior into a shared helper just because it looks
  similar. Avoid speculative frameworks, trivial wrapper layers, and excessive
  fragmentation into tiny functions that make the flow harder to follow.
- Make dependencies and ownership clear. Prefer explicit inputs and results,
  limited mutable state, and early returns that reduce unnecessary nesting.

## Correctness and finishing

- Validate external inputs at boundaries. Check sizes and ranges before indexing,
  slicing, allocating, or narrowing values. Handle failures explicitly and provide
  useful error context; do not silently swallow errors.
- Use the language's resource management conventions, including RAII in C++.
  Prefer standard containers and ownership types over manual resource management.
- For formatting or readability refactors, preserve behavior and public contracts.
  Keep changes focused and preserve unrelated work.
- Use the existing formatter and relevant lint/build/test checks when available.
  Review the diff yourself; a formatter cannot establish good responsibilities,
  meaningful names, or correct behavior. Report checks that could not be run.
- Before finishing, expand compressed statements, check blank lines and wrapping,
  remove accidental duplication, and verify that comments explain useful intent.
  Do not consider code finished just because it compiles.
- Do not hand-edit generated or third-party sources for style. Change the
  generator or template when generated output is the source of the problem.

## C++ readability example

Avoid:

```cpp
auto length = read_length(bytes); if (length < kHeaderSize) throw std::runtime_error("Invalid frame"); consume(bytes.first(length));
```

Write:

```cpp
const auto frame_length = read_length(bytes);
if (frame_length < kHeaderSize || frame_length > bytes.size()) {
    throw std::runtime_error("Invalid frame length");
}

consume(bytes.first(frame_length));
```

Use the same multiline layout for function definitions and error paths. Prefer
clear intermediate variables over packing an entire operation into one line.

## Live game navigation

- When controlling the running game, focus its window and use the game's displayed
  keyboard shortcuts to navigate login, realm selection, character selection,
  dialogs, and world entry. Do not click these controls when a key is shown.
- Read the current game screen before sending input, then verify the next screen
  or server event. Send short repeated key presses when a single simulated press
  misses the game's input polling window. Use mouse input only when a game
  action has no usable key.
- Keep live trials moving promptly; arm any required watcher before the key press
  that starts a world connection.

## Evidence and data search map

- Project Chronicles Team's standing human order is `docs/chronicles-team-standing-order.md`.
  Combat and the reported skill-use failure come first; maintain the workflow
  tracker `protocol/implementation-backlog.json` and continue eligible catalog
  work between human orders. Manual Stop/Pause always wins.
- Start with `README.md`, `NATIVE_PROTOCOL.md`, and `VALIDATION.md` to determine
  which client behavior has been observed and which is only a local hypothesis.
- Search `protocol/` for recovered message descriptors and opcode mappings.
  `docs/packet-capture.md` explains the two-way world packet format and the
  capture tool. Search private captures under `out/captures/`; never
  commit raw captures or print authentication payloads.
- Read `docs/capture-corpus.md` for the three new 2026-10-24 capture sets
  (another tutorial and two open-world parts, approximately level 8 reported
  by the human), their verified hashes/counts, and the now-finished IDA
  auto-analysis snapshot. Check this newer evidence before reusing older gaps.
- The extracted client source corpus is
  `G:\Dumps\Archeage\ClientData_2026-10-23\`. Search `Catalog/Tables/` for
  item, character, skill, map, and other CMS rows; `Catalog/` for NPC and
  spawner indexes; `Protocol/` for recovered protocol inputs; and
  `LivePackets/` for previously captured world frames and decoded indexes.
  Read source hashes and build metadata before mixing records from another
  client build.
- Use `data/` for a validated server-readable export of client catalog data.
  Run `python tools/export_client_data.py --source
  'G:\Dumps\Archeage\ClientData_2026-10-23'` to populate the ignored
  `data/generated/catalog/` tree. Read `docs/client-data.md` and
  `data/catalog-manifest.json` for layout and provenance. The server accepts
  `--client-data-dir` and the launcher uses the export when present. For
  non-catalog client assets, search the ignored `data/generated/raw/` copy and
  check `data/raw-export-summary.json`. Rebuild rather than copying selected
  rows by hand into server code.
- Search `../dumps/ida_research/` and `docs/world-entry-findings.md` for static
  and live world-entry research. Confirm database/build identity before using
  an address or an inferred handler role.
- For persistence, inspect `src/persistence/storage.hpp`, `src/persistence/storage.cpp`, and
  `migrations/` first. For native world behavior, inspect
  `src/world/session/native_world.*` and its tests. Keep observed packet fields separate from
  database fields that have no confirmed client message yet.
- For account separation, start with `src/auth/auth.*`, `src/auth/native_auth.*`,
  `docs/account-system.md`, and `NATIVE_PROTOCOL.md`. A native Steam identity
  claim is not password authentication. Read `docs/laptop-readiness.md` before
  changing bind addresses or advertising this server to another machine.
- For local Xigncode startup experiments, read `docs/xigncode-local.md` and
  `../dumps/ida_research/xigncode-local/FINDINGS.md`. The guarded private copy,
  shim, firewall, and launch helpers are under `tools/`; never apply them to the
  Steam installation. A title screen proves startup, not native login or world
  compatibility. Use a fresh grant for each named-account client.

## Tutorial gameplay and server execution

- When several chats share this checkout, agree on one owner for the shared
  build directory and a separate owner for live service restarts. Finish source
  registration and freeze shared headers before that build starts. Do not run
  concurrent CMake/Ninja jobs against the same output directory; stale mixed
  binaries are not valid test evidence.
- Build and test with `cmake --preset windows`, `cmake --build --preset release` and
  `ctest --preset release`. The preset uses Ninja Multi-Config with MSVC, so each new
  PowerShell terminal must first run `. .\tools\dev_shell.ps1`.
- For tutorial interactions, start with `docs/tutorial-interactions.md`,
  `src/world/features/tutorial/tutorial.*`, and `src/persistence/world_storage.cpp`. Rebuild static definitions with
  `tools/build_tutorial_data.py`; do not copy official character/session fields
  into runtime data. `tools/verify_tutorial_mapping.py` checks the private trace
  against catalog placements and NPC spawners.
- `docs/openssl-local-diagnostic.md` records the reproduced Windows XMM6
  compatibility failure and the explicit child-process portable fallback.
  Preserve the failing binary when investigating it; a fixture rewrite that
  changes register allocation does not establish a dependency fix.
- Keep definitions immutable and indexed after startup. Serialize each world
  session's mutations, keep database/file work off socket handlers, and never
  hold the shared protocol codec lock across a database operation.
- Validate target ownership, map/instance, distance, objective conditions,
  quantities, and request limits before granting rewards. Commit inventory,
  consumed-object markers, and quest progress atomically under a character
  revision before sending success. Retries and reconnects must not duplicate
  rewards. Derive monster kills and rewards from server simulation.
- Resolve accepted skill effects from server action deadlines. Client effect
  reports may be missing even when the original client plays the animation;
  they must not be required to consume an item or allowed to accelerate an
  effect. Serialize timer and packet mutations on the same session worker.
- Tutorial maps are private instances; shared-map presence must have explicit
  interest rules. Keep queues, per-character state, and action rates bounded.
  Distinguish synthetic tests from observed original-client behavior and
  measured load performance. Do not claim production movement authority or
  MMORPG throughput from replay/unit tests alone.

## Reverse engineering records and IDA persistence

- For every game function whose behavior is analyzed, write the finding back to
  the matching IDA database before considering that investigation finished.
- Give understood functions descriptive names with the `AAC_` prefix and a stable
  subsystem, message type, and operation. Prefer semantic names over addresses.
  Preserve existing meaningful names and record additional roles as aliases in
  comments; linker-folded helpers can serve several callers.
- Mark tentative interpretations with `Candidate` and state the uncertainty in
  a repeatable function comment. If only a low-level behavior is established,
  describe that behavior without inventing a game-specific role. Do not rename
  untouched functions merely because automatic analysis discovered them.
- Comments must record the observed behavior, evidence, schema/opcode or string
  anchors when relevant, confidence, and unresolved questions. Apply recovered
  argument/structure types and useful local names when supported by evidence.
- Keep cached pseudocode and a versioned function index under `dumps/ida_research/`.
  Record input SHA-256/build identity, image base, RVA, previous/current name,
  function extent, byte fingerprint, evidence, and confidence. Keep an append-only
  annotation journal so changes and existing user names remain auditable.
- Save the `.i64` database after each coherent annotation batch and before ending
  the investigation. Confirm the saved path and successful result; names held
  only in IDA memory are not a completed deliverable.
- For a new client build, use a separate database and compare semantic/string
  anchors and fingerprints. Revalidate candidate matches in the new binary before
  transferring names or types. Never reuse old addresses, RVAs, or opcode numbers
  as proof that a function still has the same role.
- Use bounded IDA MCP annotation batches. Do not pause another task's analysis,
  patch executable bytes, or replace existing meaningful names to apply naming
  conventions. Close or reopen an IDA instance only under the recovery rule below.

## IDA availability and recovery

- Use the IDA GUI with the existing database
  (`D:\PROJETOS\Archeage Emulator\dumps\ArcheAgeChronicles.memdump.exe.i64`) through
  ida-multi-mcp for analysis and annotation.
- If IDA or an IDA MCP request stays unresponsive for more than 5 minutes, close and
  reopen the IDA GUI on the same database, wait for analysis readiness, and continue.
  Save first if IDA still accepts a save. This applies even when another task opened it.
- When IDA is unavailable, or a quick read is enough, disassemble the binary file
  directly (for example x86-static-toolkit `inspect_pe` and `disassemble_file` at an
  RVA). Write the resulting findings into IDA once it is available again.
## Runtime ownership and build

This checkout contains the game server. The independent login-server repository owns
account authentication, player sessions, launcher grants and admission tickets.
Launcher and admin have independent desktop repositories. Shared library source in
this checkout is versioned here; do not add sibling-source build dependencies.

Use C++20 without compiler extensions, CMake, the pinned vcpkg manifest, Boost.Asio,
Boost.Beast, OpenSSL, Protobuf, nlohmann/json, libpqxx/PostgreSQL, spdlog and GoogleTest.
Use Python for extraction/verification and PowerShell for Windows development helpers.
Offline archive reading reuses CUE4Parse through the isolated .NET 10 adapter under
`tools/asset-extractor/`, because that existing parser supports the researched archive
format. Python owns descriptor/catalog conversion, runtime export and verification.
Build that optional adapter with `tools/build_asset_extractor.ps1`; its upstream source
and dependency versions are pinned. It is independent of the C++ runtime build.
Follow README.md and CMakePresets.json, reuse the build/cache and run relevant checks.
Keep credentials, certificates, client binaries, raw captures and build output ignored.

The default game build requires a verified private login authority. Preserve the
versioned service contract, single-use admission and fail-closed behavior. Keep remote
calls on workers and final socket admission free of blocking I/O. Current deployment
supports one login and one game process over the same database and host.

Consult VALIDATION.md, NATIVE_PROTOCOL.md and the protocol reference before claiming
client compatibility. Distinguish static recovery, synthetic service/transport checks
and observed behavior of the original client.
