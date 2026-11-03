# Project Chronicles configuration

The checkout and both packaged launcher entrypoints default to the local server:
HTTP `http://127.0.0.1:8080` and world transport `127.0.0.1:8081`.
Start the server separately using the README's development instructions.
No hosted service, distribution signing key, Discord application or community is supplied.

An adjacent `launcher-config.json` containing `{"edition":"player"}` selects that local
default. Operators can instead provide an explicit catalog:

```json
{
  "edition": "player",
  "servers": [
    {
      "id": "local",
      "name": "Project Chronicles Local",
      "server_url": "http://127.0.0.1:8080",
      "world_host": "127.0.0.1",
      "world_port": 8081,
      "patch_channel": "stable"
    }
  ]
}
```

A hosted entry must use HTTPS and supply its own 64-hex-character Ed25519 public key
as `patch_public_key`. `world_host` defaults to the API hostname; `world_port` defaults to
8081 for loopback and 8481 for HTTPS hosts. Server IDs must be unique. Do not place private
signing keys in this file. Signed game downloads and launcher updates require the operator's
own distribution catalog and matching public key, including for local testing.

Configure Discord OAuth separately with `CHRONICLES_DISCORD_OAUTH_CONFIG`. The example is
disabled. Replace the community link and enable its publication only after creating your
own community. Agent-team repository, account and Discord destinations also require setup.

The renamed applications use a separate Windows Credential Manager namespace and admin
settings directory. Existing game directories can be selected explicitly. Re-enter login
and operator settings in the new applications; the rebrand does not migrate private local
state or deploy changes to an existing service. Do not run old and renamed server builds
against the same database concurrently: the process lease names and Discord login lock
namespace changed. Database tables, numeric protocol IDs and recovered gameplay schemas
are unchanged. Original client terminology remains part of those gameplay contracts.

Historical validation records retain their original scope and dates with branding normalized.
They are not evidence of a new package release or a newly hosted service. Before publishing
under a new GitHub identity, use a clean source snapshot if the old Git history must not be
published; changing working files does not rewrite that history or remove existing online copies.

## Validation on 2026-11-02

The Release launcher, admin and server compile. The selected launcher, authentication,
credential, protocol and guild-text CTest run completed with 121 passing cases and two
environment-gated skips. Disposable PostgreSQL checks passed server ownership/fail-stop
and Discord identity persistence/concurrent-first-login behavior. The admin DOM/state
checks, 11 bootstrap rewrite cases and 11 protocol-reference generator checks passed.
Seven hosted TLS identity checks and the isolated signed self-update handoff also passed;
the latter verified corruption rejection, executable replacement and preserved player data.
No hosted deployment, installer publication or original-client gameplay trial was performed.
