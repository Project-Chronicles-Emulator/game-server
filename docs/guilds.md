# Guild membership

Guild support is partial. The emulator has an authoritative guild registry, PostgreSQL
persistence, front WebSocket requests, world invitations, guild chat, company requirements,
guild experience and levels, and guild housing. Original-client acceptance remains unfinished.
Recovered schemas and catalog values establish inputs; unobserved notification and permission
choices remain Candidate.

```text
src/
  guild.hpp / guild.cpp            # membership, permissions, invitations, cooldowns, experience
  guild_storage.cpp                # memory and PostgreSQL transactions
  native_guild.hpp / .cpp          # authenticated front WebSocket request/response adapter
  guild_world.hpp / .cpp           # world invitations, entry state and player guild labels
  guild_housing.hpp / .cpp         # guild housing requests and the housing directory link
  character_social.hpp / .cpp      # lobby/selection guild fields and deletion blockers
  native_chat.cpp                  # bounded per-character delivery and guild chat audience
  guild_tests.cpp                  # rules, protocol adapters and persistence regressions
  guild_housing_tests.cpp          # guild housing requests, notifications and departures
migrations/
  001_accounts_characters.sql      # guild tables (17) and company columns (19)
```

A front request obtains its actor from the authenticated session's selected stored character;
client payloads cannot supply the acting identity. The actor's company and position come from
`CompanyService` (see [companies.md](companies.md)). Guild operations serialize on the registry
mutex. Each accepted mutation writes its transaction before changing the in-memory registry.
Subscribers receive committed events after that mutex is released. Socket adapters translate
these events into their own transport envelopes and retain character/session ownership checks.

Guild creation, search, joinable lists, details, settings, automatic join, leave, member grades,
master transfer and disband are implemented. World invitations expire on the monotonic clock;
persisted leave/disband anchors use 100 ns Unix-epoch ticks. These units are distinct from the
nanosecond world action clock. Reconstructing the service loads durable memberships and
cooldown anchors. Character removal cascades memberships in PostgreSQL; startup repairs a
guild left without its master, or removes an empty guild.

## Company requirements

The client's GuildCreate dialogue refuses a character without a company (DialogueErrorCode
1106) or below `GuildCreatableCompanyPosition` (3, error 1107), read in
`AAC_Dialogue_ExecuteSelectionPlayerTask_Candidate`. The server applies the same rules and
answers `Guild_InvalidCharCompay` or `Guild_InvalidCompanyPosition`. A guild records its
founder's company and admits only members of that company (Candidate) whose position reaches
`join_company_position` (the client's join check, error 23035). Guilds stored before companies
existed have no company and accept anyone. A realm started without a company catalog skips all
company requirements, so guilds stay usable on minimal development servers. Roster entries
carry each member's company position, refreshed with the other display fields.

## Guild experience and levels

`GuildLevel` gives five levels; `requiredGuildExp` is the experience needed to leave a level
(10,000 to 100,000) and `maxMemberCount` the member limit (10 to 30). Experience is kept per
level like character experience; at the maximum level further experience is dropped. Each
change stores the member's contribution and the guild record, then pushes
`WSF2C_GuildExpUpdateNoti {guild_id, guild_exp, guild_lv}` to every member.

The official guild experience sources were not reachable in this build, so the emulator adds
one: a guild member earns one guild experience point per 10 character experience by default
(`GuildRules::character_experience_per_guild_experience`). Tutorial-domain experience awards
use a durable service-effect event with the captured membership and conversion rate; the
fractional remainder is persisted for that membership generation and rate. Authored shared-world
quest guild rewards use the same durable settlement path. Other activity sources have not been
shown to use this durable path and must not be described as exactly-once.

Guild notifications are best effort. A failed queue or disconnected front client may miss a
live update; reconnecting reloads the persisted guild state.

## Guild housing

The five guild housing requests (20034-20038) route through `GuildHousingCoordinator` into the
housing service with a live directory over this registry. Only the Master contracts or extends
an area (the client's `AAC_GuildHousing_Dialogue_CheckAreaAuthorize_Candidate` returns 1207
otherwise); rent comes from the Master's live progress on its world-session strand, so the
response arrives asynchronously. Residents need the company position of the lot size
(`GuildHousingSite{S,M,L}positionLv`). A member who leaves or is kicked loses its site and a
disbanded guild loses its area; the sites are packed into move packages. `GuildRes` and
`GuildJoinRes` carry the area. See [housing.md](housing.md).

The guild change commits first and its housing cleanup runs afterwards. That cleanup can fail
(storage error, version conflict, a busy session strand) or be lost at a server stop, so the
housing hub also reconciles guild housing with the guild registry at startup and in every
one-minute housing sweep: a guild site whose owner no longer belongs to the site's guild is
packed and a disbanded guild's area is removed. The guild rows are the durable record; no
separate outbox is kept. Cleanup never releases housing of a current member or of a guild the
directory still reports, so a late or repeated event is harmless, and a directory that cannot
confirm a guild leaves that guild's housing untouched. A departure that is followed by a
rejoin before any cleanup ran keeps its site.

## Deletion, operators and game masters

Character deletion reports the guild master block reason (`CHAR_DELETE_BLOCK_GUILD_MASTER`)
and refuses with `CharDeleteGuildMaster`. A member has no block reason value, so its precheck
answers `CharDeleteGuildMember` (Candidate) and the deletion is refused. A guild site holder is
refused with `CharDeleteHavingHousing`. The operator console also refuses deleting a guild
member, so an offline administrative delete cannot leave the live registry pointing at it.

GM commands: `guild_exp <delta>`, `guild_level <level>`, `guild_disband [guild_id]`,
`guild_master <char_id> [guild_id]` and `guild_reset_cooldown [char_id]`, or the matching
`GmCmdReq` oneofs. An operator disband needs no minimum age and starts no cooldowns.
The recovered `guild_exp` command has no stable operation ID. After an uncertain database
acknowledgement, wait for reconciliation and inspect the persisted guild state before retrying;
repeating the same command afterward is a new award and can apply it twice.

## Policies and limits

The shared registry is used by both front and world transports. Guild chat addresses only
connected members of the sender's guild. It cannot select an audience supplied by the client.
World presence decorates player units with guild labels; ordinary NPC units are unchanged.

Master transfer demotes the previous master to Member, a Master cannot leave, and a guild
cannot be disbanded before `GuildDisbandableTimeHours`; these are emulator policies. The
activity conversion rate and the same-company admission rule are emulator policy too.

Validation belongs to `src/world/features/guild/tests/guild_tests.cpp`, `src/world/features/housing/tests/guild_housing_tests.cpp`,
`src/world/features/company/tests/company_tests.cpp` and `VALIDATION.md`. Synthetic checks do not prove the original game
client's guild UI.
