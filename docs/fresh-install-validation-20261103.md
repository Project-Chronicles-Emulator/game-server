# Fresh installation validation — 2026-11-03

The Windows preview packages were downloaded from GitHub, their SHA-256 values
checked, and each component extracted separately. A new PostgreSQL 16 cluster and
ordinary application role were provisioned on isolated ports. The packaged main
SQL installer installed schema 44. Existing services and databases were preserved.

## Repairs found through the client

- The split login executable advertised the supported fingerprint as 64 ASCII
  characters instead of 32 raw bytes. Login's conversion now matches the existing
  game implementation. An executable-level regression rejects preview.1 and passes
  the corrected executable. The client then accepted the realm version.
- The local launcher profile incorrectly selected signed managed distribution.
  Unsigned loopback profiles now retain the selected compatible local client and
  use the configured login port. Hosted profiles retain signed distribution.
- Local bootstrap generation required tools and private extracted config absent
  from the launcher package. The package now includes source helpers and a local
  preparation command; no extracted config, certificate key or client is shipped.
- An existing private-client status check depended on the separate Steam source
  still having its old module hash. It now verifies the private executable, recorded
  original backup and installed module; installation still checks the source hash.
- The split-service package guide omitted the existing two-second local diagnostic
  TLS delay. Without it the client rejected the certificate before the guarded
  helper observed the connection. With it TLS completed and the world credential
  was accepted. A faster polling experiment was discarded.
- A map rendered without a catalog, but health/stamina and combat initialization
  were incomplete. Exporting the owner's matching private snapshot, generating
  tutorial/combat definitions, then starting with `--client-data-dir` repaired the
  visible bars and enabled weapon actions. The release now includes the runtime
  generator source and documents these required inputs.

## Observed local-client result

The researched `hotfix-26.10.18.734` compatible private client reached realm and
saved-character selection through the separate login/game processes. With the
generated private catalog/tutorial profile it entered map 8000, displayed health
and stamina bars, remained connected, and performed weapon actions. The original
client sent StartSkill requests for CMS skills 1000000 and 1101; the server handled
them and continued answering keepalives. Root-motion position changes were observed.
The world helper confirmed both network threads and restored the original SSL field
and two-byte startup instruction with readback. Admin connected to the isolated
game endpoint and PostgreSQL-backed account/character state.

This is a bounded local tutorial entry/action trial, not a complete tutorial,
damage encounter, health-loss/recovery, full-world population or all-skill test.
Numbered item shortcuts can remain empty without assigned items; their appearance
alone does not establish unavailable weapon skills. No new hosted-client trial or
unmodified stock-client compatibility is claimed. Generated profiles and game
inputs remain private and are not release assets.

## Reproduction

Follow [preview packages](guides/preview-packages.md) and the full
[private data pipeline](guides/game-data.md). Start login first, then game with
the packaged protocol directory, private service key, operator token, world TLS
certificate/key, catalog and both local diagnostic options. Prepare launcher inputs
from the matching extracted config and public certificate. Connect admin to game,
sign in through the launcher, and use the game's displayed selection shortcuts.

Check server startup for `Client catalog loaded` and `Tutorial definitions loaded`.
Confirm TLS completion, accepted world credential, map completion, actual character
HUD, an action and continued keepalives. Do not treat health endpoints or a rendered
backdrop as equivalent to this acceptance.

## Complete local profile generation follow-up

The initial export omitted server-owned NPC and monster populations and nine auxiliary
runtime profiles. The full exporter now regenerates these from the same hash-verified
private raw extraction. The isolated server successfully loads 56 civilian maps (2,523
units), 204 monster maps, 119,474 gathering points, 789 gathering doodad points, 3,215
loot NPC definitions, 111 processing stations, 233 recipes, five housing doodad types,
612 civilian reaction definitions, 82 adventure maps and 212 travel maps (615 volumes).
Both authored channel indexes load. Catalog verification passes for 286 tables and
11,720 raw files. No missing-profile startup warnings remain. Character items and quest
progress are unchanged across this data-only restart. These are generator and runtime
loader checks, not original-client acceptance of every generated mechanic.

The followup tutorial combat repair binds recreated player vitals to the successfully
loaded map without attaching shared monsters. Its regression fails before the repair
and passes afterward; the combat executable reports 355 passed, 19 skipped, zero failed.
The local client and user confirmed the bed/cutscene route reached the overworld; this
does not establish acceptance of every combat action or NPC interaction there.
