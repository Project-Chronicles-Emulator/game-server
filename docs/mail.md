# Server mail

The server delivers text, catalog items and currencies to a selected character or to an
account within one realm. Account mail has one shared claim receipt: the first owned
character to claim receives the attachments. Other characters can see that the mail was
already claimed. Delivery, confirmation, deletion and attachment claims persist in memory
for disposable servers and in PostgreSQL for durable servers.

This is the server-to-player mailbox. It does not implement player-to-player composition,
paid-shop purchases, the separate paid-shop storage box, bank storage or purchase history.
Those are separate protocol surfaces even though the coverage catalog groups their names.

## Delivery and operator input

`POST /admin/v1/mail` with the operator Bearer token accepts an explicit account ID, realm ID and optional
character ID. Omitting `character_id` or using zero addresses account mail. The account must
exist; a character recipient must belong to that account in the requested realm.

```json
{
  "account_id": 1,
  "character_id": 2,
  "world_id": 1,
  "delivery_key": "welcome-package-v1",
  "type": 1,
  "title": "Welcome package",
  "text": "Your starter supplies are attached.",
  "retention_seconds": 2592000,
  "attachment_retention_seconds": 2592000,
  "attachments": [
    { "kind": "item", "cms_id": 15010403, "count": 5 },
    { "kind": "money", "cms_id": 0, "count": 100 }
  ]
}
```

The example contains synthetic identifiers, not captured player data. Check the current
catalog before choosing an item ID. `type` is 0 for GM and 1 for System. Currency `cms_id`
is the recovered EX5MoneyType number: 0 Gold, 1 HersanderToken. Attachment quantities and
currency amounts are positive 32-bit integers, matching MailAttachment. An equipment
attachment can include `grade` from 0 Normal through 5 Mythic. Stackable and housing items
use their catalog representation and do not accept a grade override.

Every delivery requires a stable `delivery_key`, unique within the recipient account and
realm. Retrying the same key and content returns the existing mail, including after claim
or deletion. Changing the recipient, title, body, attachment list or retention durations
under an existing key is refused. Request timestamps can advance on a retry without
creating another reward. Use a new key for a separate intended delivery.
Deleting a recipient character cascades its character-addressed mail and frees its mailbox
capacity. Account-addressed mail and its receipt survive deletion of the claiming character,
so a sibling character cannot receive those rewards again.

The delivery planner reuses ShopCatalog's immutable item definitions and storage categories.
Unknown or unsupported item categories are refused. Successful delivery sends
MailInDeliveryNewRes (174) to the relevant online recipient; reconnecting players obtain
their durable mailbox through the REST list route.

## Tutorial-completion welcome mail

Every account receives one free welcome mail when one of its characters finishes the
tutorial. Finishing means the server-played end cutscene's map move into the overworld
channel; a GM teleport or a move between tutorial instances does not count. The mail is
account-addressed (`type` System), so the first character to claim it receives the
attachments, and it carries 20 Health potions (item 40101101) and 10 Stamina potions
(item 40301201). Both are ordinary stackable consumables that the tutorial itself hands
out. It expires after 90 days, with the same attachment window.

This is an emulator policy, not recovered official behavior. The reward definition lives in
`src/world/features/mail/tutorial_completion_mail.cpp`. Delivery uses the stable key
`tutorial-completion-supplies-v1`, so it is idempotent per account and realm: a second or
third character finishing the tutorial, a reconnect or a retry creates nothing, including
after the first mail was claimed or deleted. The online player is announced with
MailInDeliveryNewRes (174, `account_mail`) right after the map-change frames, and only when
this call created the mail. A storage failure or an item missing from the catalog is logged
and never blocks the map move. Because the content is part of the key, changing the reward
requires a new key version, which intentionally issues the new reward to every account once.
Accounts that already finished the tutorial before this change receive nothing until a
character completes the tutorial again.

## Native mailbox contract

Static analysis of the researched demo build establishes these routes under the front REST
address advertised by WorldEnterRes. Every route uses the front session's Bearer token;
the selected character determines account, realm and character ownership.

| Route | Method | Request | Success |
| --- | --- | --- | --- |
| `/api/front/MailboxGetReq` | PUT | Binary MailboxGetReq (`count`, `start`) | Binary MailboxGetRes, including total count |
| `/api/front/MailboxConfirmReq` | PUT | Binary MailboxConfirmReq (`mail_ids`) | HTTP 204; client ignores body |
| `/api/front/MailboxDeleteReq` | PUT | Binary MailboxDeleteReq (`mail_ids`) | HTTP 204; client ignores body |
| `/api/front/MailboxTakeAttachmentReq/{mail_id}` | POST | Empty body | HTTP 204; world ItemsUpdateNoti supplies reward changes |

PUT requests use `Content-Type: application/protobuf`. Confirmation records read state;
it does not grant attachments. Get returns newest mail first, excluding deleted, future
and expired mail. Confirm and delete batches are all-or-nothing. Deletion of unclaimed,
unexpired attachments is refused; deletion after their expiry is allowed. Tombstones
retain the delivery key and claim receipt.

The claim endpoint dispatches asynchronously to the active selected world session. It
never waits for another task in the shared worker pool. The session plans from its current
character/progress snapshot, then atomically commits inventory, currency, housing counts,
the gameplay revision and the mail receipt. It refreshes its gameplay caches before
emitting ItemsUpdateNoti (169) with the `system.account_mail_id` or `system.char_mail_id`
reason. Stack and money notifications contain the resulting totals. The REST completion
is delivered at most once. No active world session means a claim conflict; an offline
claim cannot change a live session's progress behind its cache.

## Atomicity and bounds

PostgreSQL schema version 23 adds `emulator_mail`. Per-account delivery locking serializes
the idempotency key and mailbox capacity. Claim locking orders character, then mail;
account-wide claims from different characters still serialize on the shared mail row.
The same transaction performs the revision check, item inserts and receipt update.
An inventory-slot conflict rolls the complete transaction back.
If a storage exception leaves the commit outcome uncertain, the world connection closes so
reconnecting reloads authoritative state. A committed receipt still prevents another grant;
the server does not keep a potentially stale tutorial cache alive after an ambiguous commit.

MemoryStore uses its existing character-state mutex for the same operations. It plans
inventory changes on copies before replacing any stored row. Both stores validate that
the proposed progress/item delta accounts for precisely the mail's attachments and no
unrelated gameplay mutation. A stale revision or mismatched immutable mail content grants
nothing. A repeat of a committed claim returns `already_claimed` without another grant.
Deleting a character removes its character-addressed mail in both backends. Account mail
and its shared claim receipt remain, including when the claiming character was deleted.

Bounds are explicit emulator policies, not claims about the official service: 1,000 live
messages per account/realm, 100 entries or IDs per request, 32 attachments per mail, 64
equipment instances per claim, 256 title bytes, 16,384 body bytes, and retention up to one
year. Inventory slots, distinct stackable/housing kinds, maximum stack size and currencies
reuse the existing storage/catalog limits. Expiry is exclusive: attachments cannot be
claimed when `now >= attachment_expiry_epochtime`. No automatic refund or expiry recovery
policy is invented.

## Evidence and verification

The build is `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`; the dumped executable SHA-256 is
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
Mail schemas establish field numbers, enum values and optional receipt timestamps.
Seventeen directly inspected functions are named/commented in the matching IDA database.
The annotated database was saved in place successfully with stored-sections verification
at `dumps/ArcheAgeChronicles.memdump.exe.i64`. Cached pseudocode, byte fingerprints,
previous/current names and append-only annotations are under
`dumps/ida_research/mail-20261026/`.

The source checks in `src/world/features/mail/tests/mail_tests.cpp` cover ownership, idempotent delivery, stale
revisions, exact attachment delta validation, durable receipt behavior, account claims,
concurrent memory claims, expiry boundaries, deletion safety, capacity, unknown catalog
items, operator parsing, native reward packet encoding, bearer REST listing and asynchronous
completion/error handling. The PostgreSQL case requires
`CHRONICLES_TUTORIAL_TEST_DATABASE_URL` pointing to an isolated disposable database.
The 2026-10-26 Release run passed all seven non-PostgreSQL mail cases, the native TLS
concurrent-claim/owner-only packet case and all 32 operator API cases. The optional
PostgreSQL mail case then passed on the isolated disposable database, within the runner's
five passing storage cases. See `VALIDATION.md` for the tested binary and wider-suite limit.
No official mail exchange or original-client emulator mailbox trial has been observed.

The isolated HTTP/TLS flow is `python tools/mail_smoke_test.py <server executable>`.
It accepts `--client-data-dir` (default `data/generated/catalog`) and `--openssl`.
The script creates temporary TLS credentials and operator tokens, starts its own memory
server on private loopback ports, exercises the real operator/router/native REST path,
and terminates only that child process. An adjacent tutorial export exercises the active
tutorial session's progress-cache adoption. Failure logs stay under ignored
`out/mail-smoke-failures/`; tokens and passwords are never printed.

On 2026-10-26 the isolated HTTP/TLS mail smoke passed against the rebuilt
`out/build-vehicle/src/Release/chronicles-server.exe`, the verified real catalog and the
adjacent tutorial profile. This includes two consecutive independent claims on the same
active world session, proving that the second reward uses the newly committed revision
and accumulated currency balance. It also verifies operator delivery and authorization,
174 notification, native mailbox listing/confirmation, ownership, safe deletion,
169 item/currency updates, duplicate-claim suppression and a tombstoned delivery retry.
The process used the documented diagnostic `OPENSSL_ia32cap=0` override; it was not changed
globally. The sanitized result and binary-hash receipt are under
`out/mail-chat-validation/mail-smoke.log` and `mail-smoke-receipt.json`.
This is synthetic network verification, not original-client acceptance.
