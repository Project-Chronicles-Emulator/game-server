# Standalone game service package

This package requires the autonomous login service running on the same host and
using the same PostgreSQL database. Configure CHRONICLES_DATABASE_URL and the same
private CHRONICLES_AUTH_SERVICE_KEY_FILE for both services. Install the database
once with tools/install_database.ps1.

Start login first, then run chronicles-server.exe --config config/game.example.json.
Provide the world TLS certificate and key at the configured paths before startup.
The game/operator API defaults to 8082; world TLS defaults to 8081. Launcher login
uses the separate login service on 8080. The private authority remains loopback 8090.
Supply --admin-token-file to enable the operator API. Public remote HTTP requires TLS.

The following distribution details are retained as developer reference; combined
server startup and desktop release commands belong to the earlier source layout.

# Launcher distribution backend

The existing C++ server now serves password login, native launch grants, HTTP/WebSocket
lobby traffic and signed patch releases. PostgreSQL remains the account/character backend;
patch publishing is an offline Python tool. The launcher uses native WinHTTP with normal Windows
certificate/hostname validation. Player machines do not need Python or the extracted bootstrap
source when using a distribution profile.

Local development defaults to `http://127.0.0.1:8480` with world port `8481`. These are configuration
values, not compiled deployment addresses. The current task does not expose a public listener.

## Configuration and later remote deployment

Start with `config/distribution/backend.local.example.json`. Copy it into ignored `out/`, replace
the signing public key and use absolute paths for certificates, protocol inputs and release data.
Set `CHRONICLES_DATABASE_URL` privately; it is never a launcher setting or CLI argument. Use the
existing account-management tools on the server host for administration. Players can register
through the launcher and the registration endpoint documented below. Password reset remains
outside this launcher flow.

```powershell
cmake --build --preset release --target chronicles-server chronicles-account chronicles-launcher
pwsh -NoProfile -File tools/start_distribution_backend.ps1 `
    -Config out/distribution-backend/backend.local.json -UseLocalDatabase
```

`-UseLocalDatabase` reads the existing private cluster's credential file without printing it. It
does not start, migrate, stop or expose PostgreSQL, and it does not stop the old emulator listener.
Without that switch, supply `CHRONICLES_DATABASE_URL` in the server's environment. The persistent
schema must already be migrated through the existing workflow.

For a new installation, run `pwsh -File tools/install_database.ps1` from the backend
package after installing and starting PostgreSQL 16. This creates the application
login/database and applies the single complete schema. Set the printed connection
settings before starting the server. Existing older schemas require a separate
upgrade procedure; this package contains only the fresh-install baseline.

The server also accepts the JSON file directly:

```powershell
chronicles-server.exe --config backend.local.json
```

JSON keys map to CLI names (`public_address` → `--public-address`). Explicit CLI arguments override
config values. `config/distribution/backend.remote.example.json` contains the later deployment
placeholders:

| Setting | Local | Later remote value |
| --- | --- | --- |
| `bind_address` | `127.0.0.1` | The server's chosen listener IP |
| `public_address` | `http://127.0.0.1:8480` | `https://api.your-realm.example` |
| `world_advertised_host` | `127.0.0.1` | `world.your-realm.example` |
| `port` / `world_port` | `8480` / `8481` | Reachable API/world ports |
| `http_tls_cert` / `http_tls_key` | Not needed on loopback HTTP | API certificate chain and private key |
| `tls_cert` / `tls_key` | Local world certificate | Certificate chain/key for the advertised world host |
| `patch_dir` | Local published directory | Published immutable release tree |
| `world_name` | `Project Chronicles Private Server` | Realm name shown in the client and launcher |
| `world_capacity` | `500` | Maximum characters connected to the world service |
| `world_label` | `none` | `none`, `new` or `recommended` |
| `world_maintenance` | `false` | `true` refuses realm entry while listed as GM-only |
| `world_char_creation_restricted` | `false` | `true` closes the realm to new characters |

See the README's realm list section for the population and state policy.

Non-loopback listeners require HTTPS and strict named-account launch grants. Legacy platform
identity mapping and local TLS diagnostics cannot be enabled there. Plain HTTP origins outside
loopback are rejected by both the server and launcher. Direct HTTPS and WSS terminate in the
existing Boost.Beast server; no replacement service or runtime is needed. The world listener
continues to use TLS and account-specific, single-use handoff credentials.

## Publish game files

Keep the Ed25519 private key outside the release input and the served directory. Generate it once
and back it up privately. The public key is embedded in the launcher distribution profile.

```powershell
$openssl = 'C:/Program Files/Git/usr/bin/openssl.exe'
& $openssl genpkey -algorithm ED25519 -out out/distribution-backend/keys/signing.key

python tools/make_bootstrap_template.py `
    --config-source out/native-config --api-origin http://127.0.0.1:8480 `
    --output out/distribution-backend/bootstrap.template.bin

python tools/publish_release.py `
    --source 'D:/PreparedClientRelease' --output out/distribution-backend/releases `
    --private-key out/distribution-backend/keys/signing.key `
    --api-origin http://127.0.0.1:8480 --channel stable --version 0.1.0 --sequence 1 `
    --bootstrap-template out/distribution-backend/bootstrap.template.bin --openssl $openssl
```

`PreparedClientRelease` must be a curated client distribution tree. No game binaries, active Steam
installation, player credentials, saves or captures are collected automatically. A small fixture
release used by tests proves updater behavior; it is not a complete game distribution.

The publisher streams files into SHA-256-addressed immutable blobs and atomically replaces the
channel manifest only after signing it. A single publisher lock prevents overlapping releases.
Every new release requires a higher positive sequence. Reusing a sequence for changed content
or downgrading an installed sequence is rejected. After changing an API hostname, regenerate the
bootstrap template and publish a higher sequence bound to that origin; update the launcher
profile as well. Do not reuse a local template for a different hostname.

The signed bootstrap template contains a fixed-length empty grant placeholder and no account
credentials. For each launch, the native launcher verifies the template hash and substitutes one
fresh grant in the verified ASCII FString. It preserves the template layout and certificate
validation. The original client's protected remote world startup remains a separate compatibility
trial; this implementation does not extend the loopback memory helper to remote hosts.

## Endpoints and trust

- `GET /health`: service and patch availability.
- `POST /emulator/v1/login`: password login, followed by authenticated launch-grant issuance.
- `POST /emulator/v1/launch-grant`: account-specific bootstrap URL. Initial activation is
  limited to five minutes; the activated URL is a 24-hour reconnect bearer credential. Redact
  it from reverse-proxy access logs as well as application logs; see [account-system.md](account-system.md).
- Existing native auth and lobby routes, including HTTPS WebSocket upgrades.
- `GET /distribution/v1/channels/<channel>`: signed UTF-8 manifest envelope.
- `GET /distribution/v1/blobs/<sha256>`: streamed immutable blob.

Manifest envelopes contain an exact `payload` string and hexadecimal Ed25519 `signature`.
The signed payload binds application, format, origin, channel, version, monotonic sequence,
file paths, sizes, hashes and optional bootstrap template. The server verifies manifests before
serving them; the launcher independently verifies them against its pinned key. Blob downloads
carry no bearer token. Redirects, cookies and ambient Windows authentication are disabled for
launcher requests. HTTPS certificate errors are never ignored.

## Installation and recovery

Choose an empty dedicated game directory for the first managed install. Existing unmarked Steam
directories are rejected rather than silently adopted. Managed installs contain
`.chronicles-install.json`. The installer rejects traversal, drive paths, alternate data streams,
Windows device names, case collisions, file/directory conflicts, symlinks and junctions. Files not
listed in a release remain untouched.

Downloads stream to `.chronicles-update/`, with exact size and SHA-256 verification before any
installed file changes. Existing files are backed up and a flushed journal is written before
same-volume replacements. A failed commit rolls back files already replaced. The next run recovers
an interrupted commit before planning updates; unexpected external file changes stop recovery.
The game must stay closed during installation. Cancellation is supported during download;
an in-progress commit is finished or rolled back as a unit. Verified staged blobs can be reused
on retry; incomplete downloads restart. Completed backups are removed after commit, and the
verified blob cache is capped at 1 GiB. Failed transactions retain their recovery data. Cleanup
failures are reported in launcher activity while the verified installation remains usable.

The gold strip reports actual downloaded bytes and installation state when patching is configured.
The launcher checks and applies updates on startup after a game directory is saved, and checks
again before account login. A changed or corrupt file is repaired from its signed release.

## Launcher account sessions

The account menu authenticates before game installation is required. Password login with
`client: "launcher"` creates a launcher session that survives native grant consumption and game
session rotation. Existing callers without that field retain their prior login behavior.
Signing out a launcher token revokes that token and pending grants while preserving a game
session already created from a grant. Game logout likewise preserves the launcher login. A new
password login still revokes older sessions for that account.

The native launcher keeps its token out of the WebView and ordinary settings. Opt-in persistent
sign-in stores it in a separate Windows DPAPI record scoped to the Windows user and exact realm
origin/account. Remember password uses the same protected native store. Sign out and Forget
saved login remove this record; packages never include it. See `launcher/DISTRIBUTION.md` for
session-expiry and automatic renewal behavior.
It validates against the existing authenticated worlds endpoint every 30 seconds and before
issuing a launch grant. HTTP 401/403 clears the expired session; enabling both persistence options
allows password sign-in again. Otherwise it prompts sign-in. Connection failures
retain the in-memory identity with an offline indication and block launching until validation
succeeds. Closing the launcher clears its in-memory credential. Explicit sign-out attempts remote
revocation, then clears the local credential even if the realm is unavailable. The server's
existing session expiry remains authoritative.

## Player registration

Player registration uses `POST /emulator/v1/register` with JSON `account` and `password` fields.
It returns HTTP 201 and `{"created":true}` after committing the account through the existing
AuthService/Store. PostgreSQL uses the existing accounts table, salted password hashing and a
unique normalized account name; no registration migration is needed. The endpoint returns 400
for invalid fields, 409 for an existing name, 413 above 4096 request bytes, 415 for non-JSON input,
and 429 after ten registration attempts in a 60-second server-wide window. Registration attempts
also count toward the existing 120-attempt credential ceiling. The limiter does not hold its lock
while deriving password hashes. Existing public-ingress rate-limit guidance still applies.
Registration does not issue a token, grant a role, enter a world or change another account's
session. Players sign in through the existing launcher login flow after creating an account.

The isolated persistence check is `python tools/registration_smoke_test.py <server.exe>
--pg-bin <PostgreSQL/bin> --launcher-tests <chronicles-launcher-tests.exe>`. It uses a new private
PostgreSQL cluster and backend, never the local player's database or running game service.
For an isolated server link, `tools/package_backend.ps1 -ServerExecutable <server.exe>` packages
that selected executable and verifies its hash without replacing the executable used by a live
trial. The normal build-directory packaging behavior remains the default.

## Stored launcher news

News is stored in the server's PostgreSQL database in `emulator_news`. Apply the updated
idempotent `migrations/001_accounts_characters.sql` through the existing `--migrate` option.
Memory mode keeps disposable news only. No sample articles are inserted by startup.

`GET /launcher/v1/news` is public and read-only. It returns at most 40 published articles,
newest publication first, with update time and id breaking ties. Drafts are excluded.
The launcher loads this feed from its configured API origin at startup and every five minutes;
**News → Refresh news** checks immediately. On failure it retries after 30 seconds and retains
the last successful feed in memory. Changing API origins clears those previous articles.
Home shows four articles; News shows the full feed. Article bodies are plain text, never HTML.

Use the operator executable on the backend machine with `CHRONICLES_DATABASE_URL` configured
as for the server. Database credentials are never distributed with the player launcher.

```powershell
# Copy and edit the supplied template. published:false saves a private draft.
Copy-Item config/distribution/news.article.example.json announcement.json
./out/build/windows/bin/Release/chronicles-news.exe put announcement.json
./out/build/windows/bin/Release/chronicles-news.exe list

# Set published:true in the JSON, then put the same id to publish or update it.
./out/build/windows/bin/Release/chronicles-news.exe put announcement.json
./out/build/windows/bin/Release/chronicles-news.exe withdraw welcome-to-chronicles
```

The backend package includes `chronicles-news.exe`; use `./chronicles-news.exe` there.
Updates replace an article atomically by its stable id and preserve its publication date.
Withdrawal retains a draft; republishing assigns a new publication date. The server supplies
timestamps, so editors do not need to enter them. Supported artwork names are `citadel`,
`voyage`, `company`, and `discover`, using the launcher's bundled artwork. UTF-8 field limits
are 80 bytes for id (lowercase letters, digits, hyphens), 40 category, 160 title, 400 summary,
and 16000 body. The operator tool reports invalid articles without saving them.

## Store, characters and community

`GET /launcher/v1/content` serves the published shop catalog and community links from
`emulator_launcher_content` (schema version 8 in the existing startup migration). Entries
are ordered by `sort_order`, update time and id, with 40 per section. Drafts stay private.
The launcher refreshes every five minutes and provides manual refresh buttons. Store search
and category filters work locally over the received catalog. Purchases and checkout happen
at the configured shop URL in the player's default browser.

The supplied `config/distribution/community.discord.json` contains this realm's Discord invite.
Publish it with the operator tool after applying migrations:

```powershell
# Uses the same CHRONICLES_DATABASE_URL as the server.
./out/build/windows/bin/Release/chronicles-content.exe put community config/distribution/community.discord.json
./out/build/windows/bin/Release/chronicles-content.exe list community

# Edit this draft with real product details and your shop URL; set published:true.
Copy-Item config/distribution/store.offer.example.json collection.json
./out/build/windows/bin/Release/chronicles-content.exe put store collection.json
./out/build/windows/bin/Release/chronicles-content.exe withdraw store your-collection
```

In a backend package, use `./chronicles-content.exe` and the adjacent `config/` templates.
The shop template is unpublished; startup never invents products, prices or destinations.
`published` is a boolean, `sort_order` is -1000 through 1000, and ids use 1-80 lowercase
letters, digits or hyphens. Title and summary limits are 160 and 400 UTF-8 bytes. Store entries
also require `category` (40 bytes), `price_label` (up to 60 bytes, may be empty), and one of
the bundled art names (`citadel`, `voyage`, `company`, `discover`). Community `type` is
`discord`, `forum`, `support`, or `website`. URLs require HTTPS, except loopback HTTP for
development, with no credentials, whitespace or executable protocols. Players submit only
entry ids when opening links; the native host resolves them from its validated feed.

`GET /launcher/v1/characters` requires a valid account bearer token and returns only that
account's saved roster for the configured realm. It does not enter a world, select a character,
rotate a token or issue a handoff. The launcher displays real names, levels, classes, experience
and equipped slots, refreshing every minute. Account changes, sign-out and expired credentials
clear the native roster immediately. Failed refreshes retain the same account's last roster
with an error indication. Character creation/selection use the game's existing screen.

```powershell
python tools/portal_smoke_test.py out/build/windows/bin/Release/chronicles-server.exe `
    out/postgresql-tools/pgsql/bin `
    --launcher-tests out/build/windows/bin/Release/launcher/chronicles-launcher-tests.exe
```

## Package and validate

```powershell
pwsh -NoProfile -File tools/package_launcher.ps1 `
    -DistributionProfile out/distribution-backend/releases/launcher-config.json
pwsh -NoProfile -File tools/package_backend.ps1

ctest --preset release -R 'Distribution|SignedRelease|PatchInstall|LauncherAccount|AuthRedirect'
python tools/distribution_smoke_test.py `
    out/build/windows/bin/Release/chronicles-server.exe `
    out/build/windows/bin/Release/launcher/chronicles-launcher.exe --openssl $openssl

# News persistence and the launcher's actual WinHTTP reader, in an isolated cluster.
python tools/news_smoke_test.py out/build/windows/bin/Release/chronicles-server.exe `
    out/postgresql-tools/pgsql/bin `
    --launcher-tests out/build/windows/bin/Release/launcher/chronicles-launcher-tests.exe
```

The backend package includes native executables, runtime DLLs, protocol inputs, migrations and
configuration examples. It excludes database credentials, certificates/private keys, release
signing keys, game assets and account data. Copy the generated `launcher-config.json` beside the
player executable through the packaging option. Those origin/key/channel values override player
settings, and the local memory/server helpers are disabled in that distribution profile.
The managed launch target starts `X5/Binaries/Win64/ArcheAgeChronicles.exe` from the verified game
directory, and requires that executable to be listed in the signed release. It does not redirect
the launch to a separate Steam installation or invoke development PowerShell helpers. The native
process receives its Steam app/game identifiers in a child-only environment; existing platform
and anti-cheat startup requirements remain unchanged.

Integration tests use disposable memory accounts and a synthetic release. They exercise actual
WinHTTP downloads, update/repair, tamper rejection, retry, HTTPS password login and authenticated
WSS. A test certificate is trusted only by the Python test process; Windows rejects it. These
checks do not prove a real original client can enter a remote world, nor that a public deployment
has been configured, signed or load-tested.
