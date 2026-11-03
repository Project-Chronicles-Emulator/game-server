# Discord sign-in

Discord sign-in uses an operator-configured Discord application, with authorization code,
PKCE S256, an expiring one-use state and a separate launcher polling secret. It requests
only `identify`. It does not use the bot token, read email, send messages or assign roles.
Discord credentials are entered in the user's normal browser, never in the launcher.

## Server configuration

Copy `config/discord-oauth.example.json` to a private file outside the served release tree,
for example `C:\Chronicles\private\discord-oauth.json`. The template is disabled by default.
Confirm the application ID, enter that application's OAuth client secret privately, and
set `enabled` to `true`. This is the OAuth client secret, not the Discord bot token.
Never include this file in the launcher, game distribution, Git, screenshots or chat.

In the Discord Developer Portal, add these exact OAuth redirect URLs as needed:

- Live: `https://api.chronicles.example/emulator/v1/discord/callback`
- Local: `http://127.0.0.1:8080/emulator/v1/discord/callback`

Each backend's `redirect_uri` must match its own registered address. Run the backend with
`CHRONICLES_DISCORD_OAUTH_CONFIG` pointing to its private configuration file. For example, set
`$env:CHRONICLES_DISCORD_OAUTH_CONFIG = 'C:\Chronicles\private\discord-oauth.json'` in its existing
private startup script before starting the server. Apply the normal additive database
migration, including schema version 30, before enabling this feature.

No Discord application or hosted service is supplied by this repository. The operator's
startup wrapper sets the configuration path for the backend process. The app secret
must be excluded from the release payload. Migration 30 adds the identity table without
resetting existing accounts, characters or exploration data.

## Player flow

An enabled server advertises Discord through `GET /emulator/v1/auth-options`. The launcher
shows **Continue with Discord** on its account panel. Authorization completes in the
browser, which returns a one-use confirmation to a temporary listener on that PC
(`127.0.0.1`, an automatically selected port). No firewall or player configuration is
needed. Only then can the native launcher redeem the result for a normal Project Chronicles launcher
session. Account creation and linking also wait for this confirmation. Keep me signed in stores
only that Project Chronicles session in Windows Credential Manager for the current Windows user, without saving
the Discord access token or app secret. Expired sessions require fresh authorization.

To preserve an existing account and its characters, sign in normally first, then choose
**Link your Discord account**. A verified Discord ID can belong to only one Project Chronicles account,
and each Project Chronicles account can have only one Discord ID. Names and emails never establish
ownership. A first Discord login without a link creates a separate player account with a
generated Project Chronicles account name. Existing account bans and native launch-grant rules still apply.
Unlinking identities is not exposed in this version.

## Runtime limits

Attempts expire after five minutes; at most 256 are retained. Callback state and successful
poll completion are one-use. The polling secret never appears in browser URLs or WebView
state. Issuer, app ID, identify scope and stable Discord user ID are checked server-side.
The browser callback carries only the expiring state and local confirmation proof, never
Discord tokens, Project Chronicles sessions or provider profile values, and suppresses referrers.

The Windows backend uses WinHTTP only for its fixed outbound Discord HTTPS requests,
to retain Windows certificate and hostname validation. Inbound routing remains Boost.Beast.
Redirects, cookies and Windows identity forwarding are disabled. Other platforms require
an outbound transport implementation before deployment. Offline transport tests establish
the local contract; they do not prove a real Discord authorization has succeeded.

Official reference: https://docs.discord.com/developers/topics/oauth2

## Local verification

On 2026-10-27 UTC, Release builds of the focused backend and launcher test targets
passed. Ten backend OAuth/password cases, 26 launcher/account/saved-login cases and
14 existing native authentication regressions passed (50 total). OAuth cases use a
synthetic Discord transport, including PKCE, issuer/app/scope checks, replay/expiry,
account ownership, bans and proof-gated linking. The launcher uses real loopback TCP
for the browser callback, including wrong-state rejection and retry. The backend and
launcher entry points compile with zero warnings/errors; active executables were not
replaced. JavaScript syntax and Git whitespace checks pass.

An isolated PostgreSQL fixture applied migration 30 and passed durable identity
reconnect and concurrent first-login checks. Its disposable cluster was stopped;
existing databases and live services were not changed by those isolated checks. They
did not exercise real Discord authorization or deployment; later live evidence follows.

Reproduce the focused checks:

```powershell
cmake --build --preset release --target chronicles-discord-tests chronicles-auth-flow-tests chronicles-launcher-tests
& out/build/windows/bin/Release/chronicles-discord-tests.exe --gtest_filter=-DiscordPostgres.*
& out/build/windows/bin/Release/chronicles-auth-flow-tests.exe
& out/build/windows/bin/Release/launcher/chronicles-launcher-tests.exe '--gtest_filter=LauncherDiscordTest.*:LauncherPasswordPolicy.*:AccountRegistrationTest.*:AccountSessionTest.*:SavedLoginTest.*'
python tools/test_discord_postgres.py --binary out/build/windows/bin/Release/chronicles-discord-tests.exe --postgres-bin out/postgresql-tools/pgsql/bin
```

## Live 0.2.1 verification

The production backend was deployed after a fresh database backup passed offsite
SHA-256 and disposable restore checks, including migration 30. The first switch
failed readiness and restored the previous backend. An isolated candidate then
started successfully on loopback-only ports. The retry finished the old Task
Scheduler wrapper before starting the new backend and passed readiness.

Live HTTPS health, auth options, news, content and signed game catalog routes return
200. Discord is enabled; public administration remains forbidden. The signed game
catalog and existing accounts, characters and exploration rows were preserved.

The operator completed browser authorization in the packaged 0.2.1 player launcher.
The existing account's Discord link was verified in PostgreSQL, then a fresh
Discord sign-in returned to that same account. The original launcher displayed its
Discord welcome message and active session. Its loopback browser return is the
temporary listener on the player's PC; it does not change the live world endpoint.
The packaged player launcher then started the original game with that Discord
session. The live world helper confirmed the selected certificate hostname,
observed both networking threads with verification result zero, and restored both
temporary changes. The server reported the world session and the operator confirmed
playable entry against the updated backend.

Focused release checks: 127 C++ cases passed and four optional fixtures skipped;
the separate PostgreSQL identity/concurrent-first-login check and 16 signed
distribution/backend/updater checks passed. These establish the stated account
and release behavior, not full gameplay or player-capacity acceptance.
