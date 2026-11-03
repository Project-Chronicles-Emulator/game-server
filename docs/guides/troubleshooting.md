# Troubleshooting a local setup

[← Setup](getting-started.md) · [Components](components.md) · [Testing](testing.md)

Start with the failing component's terminal and the exact command/config used.
Keep connection strings, access tokens and private URLs out of pasted logs.

| Symptom | Check | Next action |
| :--- | :--- | :--- |
| `cl.exe`, `rc.exe` or Ninja missing | Was `dev_shell.ps1` dot-sourced in this terminal? Is the C++ workload installed? | Load the MSVC environment; use the checked-in preset |
| CMake cannot find a package | Toolchain path, vcpkg manifest and configure output | Configure with the actual vcpkg toolchain; do not copy headers from another checkout |
| Python cannot import Protobuf | Interpreter used by CMake vs pip | Run `python -m pip install -r requirements-build.txt` with that interpreter |
| Login fails during database ownership | PostgreSQL availability, role/password/database, complete schema | Use the application role and correct database environment; do not publish the DSN |
| A second service owner is refused | An existing login/game process may still own that role/database | Keep the owner or stop it deliberately; do not kill unrelated processes |
| Game cannot verify login authority | Login running, private `8090`, same key file and database identity | Start login first; correct the pair rather than bypassing verification |
| Realm offline while login is healthy | Game process/listener and fresh heartbeats | Check game startup; login health alone is not world availability |
| World TLS files missing | `tls_cert`/`tls_key` paths resolve from the game working directory | Create the local files from setup or fix the ignored local config |
| API healthy but game client fails | Client build, data profiles, certificate policy, ordering | Follow native-protocol evidence; a health check does not establish client compatibility |
| Launcher uses an old address | Saved selection/profile may override initial defaults | Select Local, verify login `8080` and the adjacent distribution profile |
| Launcher expects downloadable files | Signed distribution is not configured in the source checkout | Select a supplied compatible local client; configure your own signed channel separately |
| Admin receives unauthorized/not found | Wrong service endpoint or no operator token enabled | Connect to game `8082`; use the same token file passed to game |
| Desktop opens blank or exits | Adjacent web assets/DLLs, WebView2 Runtime | Run from the built component directory and preserve its files |
| Exporter expects `Catalog/summary.json` | Was archive extraction/catalog building completed first? | Follow both stages in [the data guide](game-data.md) |
| Generator refuses a source hash | Snapshot belongs to another client build | Revalidate that build; do not delete the guard to force acceptance |
| Game has no NPCs or tutorial behavior | Catalog alone does not load every feature | Review profile prerequisites, bootstrap flag and explicit placement input |
| CTest reports `NOT_BUILT` | The relevant test target was not built | Build that target or the full suite before running it |
| PostgreSQL tests skip/fail setup | Missing or inappropriate isolated fixture | Read the test's preconditions; use its dedicated fixture, not a shared populated DB |

## Collect a useful reproduction

Record repository/commit, exact component and invocation, client/data build identity,
steps, expected/actual behavior and a short redacted error excerpt. For a gameplay
failure, identify the native request and the first rejection or wrong state when
possible. Keep raw captures private and describe whether the original client or a
synthetic request produced the result.

Open an issue in the owning repository using its bug template. If several components
are involved, link the reports and describe the boundary that fails.
