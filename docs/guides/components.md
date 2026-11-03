# Components and configuration

[← Documentation](../README.md) · [Local setup](getting-started.md)

## Login server

[Repository](https://github.com/Project-Chronicles-Emulator/login-server) ·
Entry point: `src/apps/login_main.cpp` · HTTP adapter: `src/services/login/`

Login owns password/Discord account authentication, session issuance and rotation,
launcher grants, realm discovery and one-use world admission. It also serves launcher
news/content and optional signed patches. It does not simulate a world.

| Setting | Local default / purpose |
| :--- | :--- |
| `config/services/login.example.json` | Copy to an ignored `*.local.json` to customize |
| `bind`, `port`, `public_address` | `127.0.0.1`, `8080`, `http://127.0.0.1:8080` |
| `game_api_address` | `http://127.0.0.1:8082`, returned as the game front endpoint |
| `authority_port` | `8090`, bound separately to loopback |
| `CHRONICLES_DATABASE_URL` | Private libpq connection string |
| `CHRONICLES_AUTH_SERVICE_KEY_FILE` | Private 64-hex-character key file shared with game |

Start with the checked-in example and environment from [setup](getting-started.md).
For remote public HTTP, configure the TLS certificate/key and a matching remote HTTPS
origin. The private authority remains same-host loopback. Player tokens cannot
authenticate private service requests; the service uses its own authenticated protocol.

Sessions are process-local. A login restart requires reauthentication; old pending
admissions cannot be reused under a new login incarnation. Activated reconnect
credentials have their own existing durable expiry/revocation contract. A world
heartbeat older than five seconds makes the realm unavailable for new entry.

## Game server

[Repository](https://github.com/Project-Chronicles-Emulator/game-server) ·
Entry point: `src/apps/server.cpp` · World session: `src/world/session/`

Game owns character state, world connections, simulation, feature services and the
operator API. It verifies login authority and database identity before startup,
revalidates sessions through login and consumes account/character/world-bound tickets.

| Setting | Local default / purpose |
| :--- | :--- |
| `config/services/game.example.json` | Service configuration, with CLI overrides |
| `bind_address`, `port`, `public_address` | `127.0.0.1`, `8082`, `http://127.0.0.1:8082` |
| `world_port`, `world_advertised_host` | `8081`, `127.0.0.1` |
| `auth_authority` | `http://127.0.0.1:8090` |
| `tls_cert`, `tls_key` | Local world TLS files; create them before startup |
| `--client-data-dir` | Hash-checked runtime catalog directory |
| `--admin-token-file` | Enables the otherwise disabled operator API |

Account credentials and authentication state belong to login. Character/world
repositories remain game-owned. The current implementation shares a PostgreSQL
schema between services; this is not independent-database or multi-world deployment.
Login and game hold distinct exclusive leases, so one of each can coexist but a
second owner of the same role is refused.

Private admission work runs on workers. The remote admission commit is the
cross-service authorization point; final socket admission checks the matching
prepared local result without blocking network/database work on the socket executor.
Keep that boundary when fixing authentication or connection races.

## Launcher

[Repository](https://github.com/Project-Chronicles-Emulator/launcher) ·
Entry points: `launcher/src/main.cpp`, `launcher/src/controller.cpp`

The launcher is a Windows WebView2 desktop application. It signs players in,
selects a local client installation and handles configured signed distribution.
For this split, start services externally and keep automatic server startup off.

For an explicit local catalog, create **beside the built launcher executable**
`out/build/windows/bin/Release/launcher/launcher-config.json`:

```json
{
  "edition": "player",
  "servers": [{
    "id": "local",
    "name": "Project Chronicles · Local",
    "server_url": "http://127.0.0.1:8080",
    "patch_public_key": "",
    "patch_channel": "stable",
    "world_host": "127.0.0.1",
    "world_port": 8081
  }]
}
```

An empty patch key is accepted for this local development entry. Hosted distribution
requires your own pinned signing identity and published channel; do not invent a key
or expect the source checkout to supply client downloads. Existing saved settings
may retain an earlier selection: select Local in the interface after changing profiles.

**Client boundary:** the researched original client is build-specific. Native message
schemas, local TLS admission or reaching a launcher screen do not prove a working
gameplay session. Client startup helpers are guarded for the researched build and
must refuse unknown binaries. Development/private-client preparation helpers under
`tools/` are optional research tooling, not a general client installer. See the
[native protocol notes](../../NATIVE_PROTOCOL.md) before changing this path.

## Admin

[Repository](https://github.com/Project-Chronicles-Emulator/admin) ·
Entry points: `admin/src/main.cpp`, `admin/src/console_client.cpp`

Admin is a Windows WebView2 operator console. Connect it to game HTTP `8082`, not
login HTTP `8080` or private authority `8090`:

```powershell
.\out\build\windows\bin\Release\admin\chronicles-admin.exe `
    --server http://127.0.0.1:8082 --token-file C:\ProjectChronicles\private\admin-token.key
```

Game must have been started with that same operator token file. The admin token,
service key and player password are three separate credentials. A saved connection
does not change automatically just because the application's initial default changed.
SSH/proxy management is optional; establish a local operator connection first.

## Configuration habits that prevent confusion

- Treat checked-in examples as public defaults. Use ignored `*.local.json` for local overrides.
- Keep keys, client files, generated data and database credentials outside tracked source.
- Keep four checkout/build directories separate; do not copy source across siblings to fix an include.
- Run one build owner per output directory and one runtime owner per service role.
- Keep public origins, advertised world hosts and certificates consistent when moving off loopback.
