# Project Chronicles Game Server

C++20 MMORPG emulator game service. Owns characters, world state, simulation,
world TLS connections and the operator API. Authentication and admission tickets
belong to the separate [login-server](https://github.com/Project-Chronicles-Emulator/login-server).
The default build requires that service and never silently creates local sessions.

## Build and installation

Use Visual Studio 2026 C++ tools, CMake 3.25+, Ninja, Python and the pinned vcpkg
manifest. Every checkout contains its source dependencies. Follow the existing
[build and data guide](docs/development-build-guide.md), building chronicles-server
and chronicles-roster-tests. Run relevant authentication tests through CTest.

```powershell
. .\tools\dev_shell.ps1
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset release --target chronicles-server chronicles-roster-tests
ctest --preset release -R "AuthAuthority|SplitAuthTest|NativeAuth|AuthService|LauncherRoster"
```

Install PostgreSQL 16 and run tools/install_database.ps1 once. The complete schema
is migrations/001_accounts_characters.sql; a fresh installation needs no update
sequence. Both services must use that same database and a shared private service
key file. Set CHRONICLES_DATABASE_URL and CHRONICLES_AUTH_SERVICE_KEY_FILE privately
in both environments. Follow the login repository's SETUP.md for key configuration.

Supply a world TLS certificate/key at the paths in config/services/game.example.json,
or override those paths in an ignored local copy. Start login first, then run
chronicles-server.exe --config config/services/game.example.json from the repository
root. The executable is in out/build/windows/bin/Release/.

Defaults: game/operator HTTP 127.0.0.1:8082, world TLS 127.0.0.1:8081, private login
127.0.0.1:8090. The admin connects to game; the launcher connects to login on 8080.
The services verify database identity and hold distinct exclusive leases, permitting
one login and one game process per database. Keep keys, passwords, certificates,
client binaries and captures out of public files. Remote HTTP requires TLS.
The operator API remains disabled until a private `--admin-token-file` is supplied.
Configure that token and the game HTTP address in the admin application.

## Validation and implementation limits

The split is checked with isolated PostgreSQL and two processes, including character
creation/selection, both TLS admission credentials, replay rejection and login
outage/restart. These are synthetic checks, not original-client gameplay acceptance.
The remote admission commit is the cross-service authorization point; final socket
admission uses its locally prepared result. Independent databases, multi-host private
transport and login-session replication are not implemented.

This emulator remains under development. Recovered protocol inputs describe one
researched client build. Local extracted game data is required for catalog-backed
features; proprietary client binaries and dumps are not distributed here.
Read [feature coverage](docs/protocol-feature-reference.md), [validation](VALIDATION.md)
and [native protocol notes](NATIVE_PROTOCOL.md) before assuming compatibility.

Related repositories: [launcher](https://github.com/Project-Chronicles-Emulator/launcher),
[admin](https://github.com/Project-Chronicles-Emulator/admin).

See [service split validation](docs/service-split-validation.md) for completed checks and limits.
