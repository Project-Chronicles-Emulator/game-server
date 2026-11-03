# Character persistence

## Database installation

Install PostgreSQL 16 and PowerShell 7.2 or later. With PostgreSQL running, execute
this one command from the repository's `Emulator/` directory or the backend package:

```powershell
pwsh -File tools/install_database.ps1
```

The installer finds `psql` on PATH, in the local development tools, or in the standard
Windows PostgreSQL 16 directory. Supply `-PostgresBin` for another installation.
Defaults are host `127.0.0.1`, port `5432`, administrator `postgres`, database
`chronicles` and application login `chronicles`. Use `-DatabaseHost`, `-DatabasePort`,
`-AdminUser`, `-DatabaseName` and `-DatabaseUser` to override them.

Passwords are prompted securely. Automated callers may pass `-AdminPassword` and
`-DatabasePassword` as SecureString values. Administrator authentication can also
use `PGPASSFILE` or `PGPASSWORD`. New application passwords require at least 12
characters. The script creates a dedicated application login without superuser or
role/database creation privileges, creates its owned database, and installs all
tables, constraints, indexes and initial rows. Passwords are not printed or saved.
It prints the server connection settings; supply the application password through
PostgreSQL's password file or `PGPASSWORD` when starting the server.

`migrations/001_accounts_characters.sql` is the only installation SQL file. It
directly creates the final schema, equivalent to the former schema 44, without
replaying old upgrades or data backfills. Historical version markers remain for
runtime health-check compatibility. The experimental schema 45 is not installed.

The schema uses one atomic PostgreSQL block and the same transaction-scoped advisory
lock as `migrate_postgres`. Repeating installation against a complete current schema
preserves accounts and characters. An older, incomplete or unrelated schema is
rejected; the installer does not reset passwords, drop databases or upgrade data.
Role/database creation precedes the atomic schema installation, so a failed attempt
can leave an empty owned database or application role; correct the error and rerun
with the same application password.

The server's existing `--migrate migrations/001_accounts_characters.sql` option and
`tools/start_persistent.ps1` use this same schema. The memory backend remains available
for disposable development. Schema-version references later in this document describe
historical implementation milestones, not separate installation steps.

## Stored state

| State | Source and behavior |
| --- | --- |
| Account and character identity | Login and character creation, with database uniqueness for names and account/world lobby slots. |
| Character appearance | The creation request's customization bytes; loaded for the lobby and world player. |
| Map, position, body yaw | Verified `PC2S_MoveReq`; checkpointed during a world session and at disconnect. |
| Level and experience | `save_progress` provides an owner-scoped write; gameplay events are not yet confirmed, so no request currently changes them. |
| Inventory and equipment | `emulator_character_items` stores stable item IDs, CMS IDs, inventory slots, equipped slots, quantity, durability, and grade. Supported class gear is inserted atomically with character creation. |
| Camera yaw and pitch | Nullable schema fields only. The recovered `MoveReq` has body yaw but no camera fields, so the server does not write camera values. |
| NPC shop purchase limits | Schema version 12 `emulator_character_shop_purchases`: purchased count and reset tick per shop row, committed with gold and stacks under `gameplay_revision`. |
| Shop-adjacent rows | Schema version 15: `emulator_character_items.created_epoch_ticks` (zero before tracking), `emulator_character_housing_items` (owned and installed counts shared with the housing service) and `emulator_character_trade_vouchers` (redeemed vouchers stay as tombstones). |

`Store::commit_item_trade` applies a revisioned `WorldProgress` transition, deletes owned
unequipped item rows, restores item durability (guarded by the expected old value) and inserts
new item rows in one transaction. NPC shop trades use it so
gold, stackables, purchase counters and item instances change together or not at all.

World movement is checkpointed at most once every two seconds during a session,
then again on a clean disconnect. An abrupt process or machine failure can lose
movement since the last successful checkpoint. PostgreSQL transactions protect
item changes, and character-plus-item reads use one repeatable-read snapshot.
The unique item-slot indexes also support per-character lookups. The world
listener caps active connections at 512 by default (`--max-world-connections`); this is
a resource bound for the 500-player target, not a measured production capacity. No concurrent-client load benchmark or
database connection pool has been completed yet.

The item table has a foreign key to characters, unique inventory slots per
character, and a partial unique index for equipped slots. Writes check account,
character, and world ownership inside parameterized SQL transactions. Item IDs
are generated by storage and remain stable through updates. Deleting a character
cascades to its items.

Version 3 previously backfilled a nine-item Wild set on empty characters.
Version 4 marks each newly created character as provisioned. On an existing
character's first owner-scoped list load, the server adds only missing starter
items for Fight, Wild, Adamant, or Magic, preserving any owned CMS item,
equipped slot, and occupied inventory slot. It then marks that character as
provisioned, so later item removal does not recreate gear.

Character creation accepts only classes with a complete catalog-backed starter
set; any other `char_class` value fails with `BadRequest`. The `Character`
table also defines Death (4) and Love (6) creation rows and each has a base
clothes pack and level-one costume items, but the `Item` table contains no
weapon either class can equip, so no playable loadout can be provisioned and
they stay rejected until one is validated. Classes 7 through 11 have no
`Character` row. Without this guard those requests spawned a gearless
character that the client displays with a default weapon.

The client catalog links each supported class to three base outfit models and
exact costume items; its class-specific level-one weapon and the common bag are
also present. An official Magic world packet confirms its four fabric armor
items. The catalog does not establish which armor material belongs to Fight,
Wild, or Adamant at creation, so new characters in those classes receive a
weapon, costume suit/gloves/shoes, and bag. Existing Wild fabric armor from
version 3 is preserved. See `src/data/starter_gear.hpp` and
`docs/packet-capture.md` for the evidence boundary.

| Class (wire ID) | Base clothes pack | Weapon CMS ID | Costume CMS IDs | Armor evidence |
| --- | ---: | ---: | --- | --- |
| Fight (1) | 471 | 11101102 | 10130102, 10130103, 10130104 | Unresolved |
| Wild (2) | 473 | 11102302 | 10130202, 10130203, 10130204 | Unresolved |
| Adamant (3) | 472 | 11103102 | 10130302, 10130303, 10130304 | Unresolved |
| Death (4) | 474 | None in catalog | 10130501, 10130502, 10130503 | Creation rejected |
| Magic (5) | 475 | 11104102 | 10130402, 10130403, 10130404 | Official packet: 11403101, 11403201, 11403301, 11403401 |
| Love (6) | 101 | None in catalog | 10130601, 10130602, 10130603 | Creation rejected |

## Current boundary

Storage supports item grants, updates, removals, position checkpoints, and
level/experience updates. The world listener must only call these APIs after a
packet has been decoded and its gameplay meaning is verified. The current
native world path checkpoints position and body yaw; item and progression
mutations are not yet observed end to end with the original client.
