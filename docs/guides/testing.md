# Testing and evidence

[← Documentation](../README.md) · [Contribution workflow](feature-workflow.md)

Use the smallest check set that establishes the changed contract, then broaden it
when a failure, changed dependency or unresolved concern justifies doing so. Reuse
existing build/cache directories; do not run competing builds into the same output.

## Build before selecting tests

The game repository defines many CTest executables. A focused runtime build does
not build all of them. Running the entire CTest inventory immediately afterward can
produce `NOT_BUILT` entries rather than a useful result.

```powershell
. .\tools\dev_shell.ps1
cmake --build --preset release --target chronicles-roster-tests chronicles-services-front-tests
.\out\build\windows\bin\Release\chronicles-roster-tests.exe
.\out\build\windows\bin\Release\chronicles-services-front-tests.exe
```

For the full game suite:

```powershell
cmake --build --preset release --target chronicles-tests
ctest --preset release
```

For the complete smaller login checkout:

```powershell
cmake --build --preset release
ctest --preset release
```

Desktop checkouts configure tests off by default. To run available launcher tests,
configure it with `-DBUILD_TESTING=ON`, build `chronicles-launcher-tests` and run that
executable. Admin changes need its standalone build plus verification of the
affected operator flow; do not infer UI correctness from compilation alone.

## Select by the owning contract

| Change | Useful checks |
| :--- | :--- |
| Login sessions, authority or handoff | `AuthAuthority.*`, `SplitAuthTest.*`, `AuthTest.*`, `NativeAuth.*`; paired-service integration |
| Lobby/front contract | `chronicles-services-front-tests`; actual native ordering where relevant |
| Persistent mutation | Feature/domain tests plus correctly provisioned PostgreSQL fixtures |
| Data generator | CLI validation, manifest/hash/readback and the consuming loader |
| World connection/lifetime | Owning net/world-session tests and synthetic TLS reproduction |
| Launcher/admin UI | Standalone build, endpoint configuration and the actual changed screen/action |
| Documentation/progress | Link/anchor review, rendered README preview and progress/reference checks |

Example narrow GoogleTest selection:

```powershell
.\out\build\windows\bin\Release\chronicles-roster-tests.exe `
    '--gtest_filter=AuthAuthority.*:SplitAuthTest.*'
ctest --preset release -N
```

Find target names in the owning module's `CMakeLists.txt`. Native runtime behavior
spans adapters, feature services, repositories and the world-session composition;
testing one helper is not proof of the whole flow.

## PostgreSQL tests need intentional fixtures

Optional integration tests skip when their environment is absent. That is not a
durability pass. Use an isolated PostgreSQL cluster/database and the main schema,
not the database used by your local player or another running service.

The environment names used by current checks include:

| Variable | Purpose |
| :--- | :--- |
| `CHRONICLES_RECONNECT_TEST_DATABASE_URL` | Durable reconnect credential checks |
| `CHRONICLES_TUTORIAL_TEST_DATABASE_URL` | Many world-storage integration checks |
| `CHRONICLES_DISCORD_TEST_DATABASE_URL` | Persistent Discord identity/concurrency fixture |
| `CHRONICLES_NEWS_TEST_DATABASE_URL` | News persistence fixture |
| `CHRONICLES_PORTAL_TEST_DATABASE_URL` | Portal persistence fixture |

Do not set every variable to a populated shared database. Read each test's setup:
the Discord fixture expects a fresh account set; the loot serialization fixture
requires a loopback database named `loot_claim_test`; inactive skill-item checks
require their experimental table; fault/restart checks require their own proxies,
stage configuration and process boundaries. The inactive fixture does not belong
in the main installer. Some historical harnesses are retired, so their dated commands
are evidence records rather than runnable tools in a fresh checkout.

Record which optional fixtures were actually supplied and which checks were skipped.
Do not disable or rewrite an assertion simply because its required fixture is missing.

## What to put in a validation report

Include the commit/build identity, client and data identity where relevant, changed
contract, exact check/reproduction, result and remaining limitation. Keep credentials,
player/session values and raw captures private. Distinguish these outcomes:

- **Build:** the selected source compiled and linked.
- **Synthetic:** a unit/domain/service/storage/transport reproduction passed.
- **Client observed:** the original client completed the stated action on the stated build.
- **Capacity measured:** a specific measured load/scenario, not an extrapolation from unit tests.

See [the recorded service split checks](../service-split-validation.md). They establish
the tested split, not a fresh full-game client trial.
