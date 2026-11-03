# Server messages and player commands

`config/server-messages.example.json` is a safe, disabled-by-default configuration.
The recurring `enabled` flag and `welcome.enabled` flag are independent. Enable either
after supplying the text you want players to see. `sender_name` defaults to `[Server]`.
The server integration loads the selected JSON file and keeps one scheduler for the process.

Start with `--server-messages-file config/server-messages.example.json` (or your own file).
The authenticated operator API supports `GET /admin/v1/server-messages` and
`POST /admin/v1/server-messages` with the complete configuration object. Successful writes
report `settings_saved`; a false value means the runtime update worked but persistence did
not. When `--admin-settings-file` is configured, saved operator settings take precedence over
the startup message file after a restart. The API retains its normal 16 KiB request limit.

Recurring messages rotate through `messages` in order, one entry each `interval_seconds`
(1–604800 seconds). `type` is `chat` or `announcement`. The first message is due one
interval after startup or replacement of the configuration. The scheduler uses a monotonic
clock: delayed ticks send one due message and start a fresh interval, without replaying
the missed intervals. An invalid configuration is rejected before replacing a valid one.
The scheduler is safe to update from the operator API while the timer and entry callbacks run.

Chat messages go to the realm's connected front-chat recipients. Announcements use the
existing world system-message banner delivery path. Welcome messages are private chat
notices to the entering character, once when the world connection starts playing. Character
selection and unrelated players do not trigger or receive that welcome. A reconnect can
receive the welcome again. Each welcome line is its own chat notification.

All configured text must be valid UTF-8, 1–1024 bytes, with no control characters or embedded
newlines. Use separate entries for multiple lines. Sender names have the same rules and a
32-byte limit. A configuration can contain at most 100 recurring entries and eight welcome
lines; files are limited to 256 KiB. Unknown keys, message types and out-of-range intervals
are rejected so configuration typos cannot silently disable intended behavior.

Player commands:

- `!help` prints seven short, private lines listing the commands.
- `!pvp on` requests the authoritative world service to enable the player's PvP flag.
- `!pvp off` requests the world service to disable it.
- `!pvp status` or `!pvp` requests the current status.
- `!mount` summons and seats a horse. Any character may use it: a registered summon is
  preferred (the lowest id), and a character without one borrows the catalog's lowest
  rideable `SummonNpc` without registering it. Refused while riding, dead, in the tutorial
  instance or inside a housing visit. See [vehicles and trade](vehicles-and-trade.md).
- `!dismount` gets off the ridden horse or trade wagon at the unit's position, exactly like
  the client's `GetOffUnitReq` without a landing point. The horse is removed; a wagon stays.

The server retains `/help`, `/pvp`, `/mount` and `/dismount` aliases for clients that forward
them. The original demo
client routes slash input through its local alias catalog and silently discards an unknown
alias. Its 236 exported aliases do not contain `/help` or `/pvp`; use `!` with the stock client.
Static routing findings are recorded in `dumps/ida_research/player-chat-commands-20261026/`.

Every slash- or bang-prefixed front-chat message is consumed before channel routing. Unknown
commands receive a private help hint; commands never reach another player's chat or the
operator's public chat history. The command dispatcher limits a character to ten commands
per ten seconds, shared across transports when the same dispatcher is used. Commands
require an authenticated selected-character identity. PvP handling belongs to the live
world session: the parser never claims a flag changed without calling that service. Mount
commands are queued on the same live world session, which sends the outcome line itself.

The scheduler, parser, private routing, limits and JSON replacement have synthetic unit
coverage in `src/world/features/chat/tests/server_messages_tests.cpp` and `src/world/features/chat/tests/player_commands_tests.cpp`. These tests
do not establish original-client rendering. The recovered front `ChatNoti` shape supports
the private server sender label; the existing world `SystemMessageNoti` free-text banner
path remains unverified in the original client. Official world captures contain no chat
frames and official system notices use a CMS key, so this is emulator policy rather than
a claim about the official server's commands or announcement semantics.

`tools/server_messages_smoke_test.py <server.exe>` exercises separate synthetic front
clients and a temporary TLS world server: private welcome only after StartPlaying, welcome
de-duplication, private help/PvP status, operator configuration persistence, recurring chat
and banners, and disabling recurring delivery. It uses a disposable in-memory store and
the emulator's provisional world credential; no running game or server is reused.

The 2026-10-26 Release run passed all five scheduler and five player-command unit cases,
all 32 operator API cases, the isolated HTTP/TLS smoke and the operator UI logic check.
The TLS smoke used the process-local diagnostic `OPENSSL_ia32cap=0`. Logs are under
`out/mail-chat-validation/`; see `VALIDATION.md` for the tested binary and full-suite
limitation. No original-client rendering or live deployment was performed.
