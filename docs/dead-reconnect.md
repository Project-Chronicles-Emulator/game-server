# Saved death on reconnect

The hosted report was a modal reading "Invalid Request" immediately after spawning.
Read-only server logs showed a saved-dead character reconnecting, sending automatic nearby
object discovery requests, and then remaining stationary. The corresponding database flag
was still dead. The reporting player's identity was not supplied, so attribution of the
screenshot to that session remains unconfirmed.

The matching client build's English localization identifies that exact text as `BadRequest`
(error 4). It uses different text for `Interaction_TargetNotFound` (4207). Before this repair,
the tear service returned `BadRequest` for every discovery from a dead character, including
the client's automatic requests during entry.

The server also restored the combat session with zero health but always announced the
player's initial state as `Idle`. It did not restore the death notification after the local
unit was ready. This left the client without the presentation needed to request revival.

The repair preserves the saved death and existing revival transaction:

- Entry and later sight announcements carry the authoritative life state and vitals.
- After the first `StartPlayingReq` for a loaded map, a dead character receives `Demise`
  followed by the existing environmental `DeathNoti` shape, with no invented attacker.
- Repeated `StartPlayingReq` does not repeat those notifications. No damage, durability
  deduction, reward, or new death transaction is replayed.
- Automatic tear/statue discovery while dead produces no reply or state mutation. Explicit
  collection, activation, offerings and interaction requests retain their refusal checks.
- Successful death and revival transitions refresh later observer announcements. Ordinary
  stat ticks do not acquire the presence lock again when life state is unchanged.
- Generic request refusals log request/response opcodes, sequence and error code without
  recording private payloads. Numeric success enums remain valid.

The passive-discovery policy does not bypass the existing request-rate bounds. It does not
implement unindexed objects, grant discovery progress while dead, or automatically resurrect
the player.

The official captures establish the live-death `Demise`/death sequence and its relation to
revival requests. All 26 captured map-entry notifications describe living characters; the
dead-reconnect presentation is a compatibility repair, not an observed official entry flow.
An original-client reconnect and revival trial remains necessary before claiming that UI
acceptance. See `combat-evidence.md` for the captured death and revival contract.

Release validation: 25 focused checks passed; the full C++ executable passed 1,036
cases with 94 environment-gated skips and zero failures (1,130 total). The native
HTTP/WebSocket/TLS smoke passed 39 checks, and 11 protocol-reference generator
checks passed. The source was independently reviewed. Deployment is recorded
separately; these checks do not establish original-client UI acceptance.

The final candidate preserves the subsequently deployed Project Chronicles 0.2.7 baseline. All five
critical CTest cases and the full rebased executable pass. Only the scoped reconnect
repair is added to that baseline; its test kit and vehicle features are preserved.

The later shared-main recovery preserves both the local commit and all incoming release
changes. Its complete Release build passes: the core executable has 1,036 passes and
96 environment-gated skips; the launcher has 122 passes and four integration skips;
the remaining CTest suites have 273 passes and 14 optional skips. Seven hosted-TLS
identity cases and all 12 integration checks pass. Five launcher fixtures explicitly
select their temporary game directory so a real running Steam game cannot affect them;
the production running-game guard is unchanged.

The complete private protocol Page was reconciled with the generated reference during
that recovery and verified by full ordered nonblank-line readback. The publication record
contains the verified document fingerprint. Original-client reconnect and revival UI
acceptance remains pending.
