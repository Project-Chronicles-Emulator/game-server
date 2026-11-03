# Companies

Companies are the game's five factions (Antiqus, BlueSalt, Fairwind, Shadowhk and YokedIron).
Support is partial: membership, contribution, position levels and pioneer permits are
authoritative and persistent, but no company NPC dialogue is reachable in the current live
profiles, so characters normally join through the operator console or a GM command.

```text
src/
  company.hpp / company.cpp        # rules from the catalog, CompanyService, WorldCharCompany
  src/world/features/company/company_storage.cpp   # company stores (schema version 33)
  src/persistence/company_permit_storage.cpp        # the MemoryStore permit commit
  company_dialogues.hpp / .cpp     # which dialogue selections join/change/register permits
  company_world.hpp / .cpp         # world packets and the pioneer permit charge
  social_gm_commands.hpp / .cpp    # guild and company GM commands
  character_social.hpp / .cpp      # lobby, selection and deletion view of company and guild
  company_tests.cpp                # rules, persistence, dialogues, packets, GM grammar, lobby
tools/
  native_social_smoke_test.py      # operator seat, lobby company, company-gated guild
```

## Rules and evidence

- `Company` rows give each company and `requireContributionToChangeCompany` (4000).
  `CompanyPosition` rows give, per company and level, the cumulative `nextLevelContribution`,
  `isMaxPositionLevel`, `changePenaltyContribution` and the pioneer permit duration and cost.
  `GameVariable` gives `CompanyChangeLimitMin` (1440) and
  `RenewPioneerPermitDurationMinuteBeforeExpiry` (1440). Fairwind and YokedIron export five
  positions; the others export six.
- **Position level** is the first level whose `nextLevelContribution` exceeds the
  contribution. This is exactly what the client computes
  (`AAC_Company_Catalog_PositionLevelForContribution_Candidate`), and the client does not read
  `isMaxPositionLevel` there.
- **Caps (Candidate):** a level flagged `isMaxPositionLevel` holds earned contribution one
  below its threshold until a `CompanyLevelUnlock` reward or GM command unlocks a level at least
  that high. Capping contribution, not the level, keeps the client's own level display in
  agreement. Each exported unlock reward (BlueSalt levels 2 and 3) lifts a real cap.
- **Join / change:** the client's dialogue prechecks
  (`AAC_Company_Dialogue_CheckJoinAllowed_Candidate`,
  `AAC_Company_Dialogue_CheckChangeAllowed_Candidate`) refuse a join with a company and a
  change before `join time + CompanyChangeLimitMin` (DialogueErrorCode 1104). The server
  applies the same rules and also requires the target company's contribution requirement and
  deducts the current position's change penalty (Candidate). A guild member cannot change
  company, because the guild belongs to its founder's company (Candidate).
- **Pioneer permits:** registration requires a company (1103) and the position's money
  (1101); extension is allowed only inside the renewal window (1102). The cost is charged
  from the character's progress with `ItemsUpdateNoti` (`company_pioneer_permit`). The
  permit and cost use one PostgreSQL transaction or the shared memory-store commit; stale
  company or progress records reject the operation. This integration is not yet validated.
- Times are 100 ns Unix-epoch ticks, the unit of `WorldCharCompany.*_time_since_epoch`.

## Requests and packets

| Path | Behaviour |
| --- | --- |
| `PC2S_DialogueFunctionReq` (20067) | Company tasks only: the selection must be a CompanyJoin, CompanyChange, PioneerPermitRegist or PioneerPermitExtend row, the session must have an open dialogue with one of the dialogue's speaker NPCs, and the company comes from the selection's `SystemCompany` condition or the NPC's `company`. |
| `PS2C_JoinCompanyRes` (92) | Accepted join or change, and GM company switches. |
| `PS2C_CompanyUpdateNoti` (93) | Contribution or position changes from activity or GM commands. |
| `PS2C_RegisterPioneerPermitRes` (94) | Accepted permit registration or extension. |
| `PS2C_CommonErrorRes` (2) | Refused company dialogue function, category `EErrorCategory_Company`. |
| `CharListRes` / `CharSelectRes` | `LobbyCharacter.company_type/company_contribution` and `WorldCharacter.company`. |

## Contribution sources

The official contribution sources (quests and activities) were not reachable in this build.
As emulator policy, a company member earns one contribution point per 20 character experience
(`CompanyRules::character_experience_per_contribution`), with the remainder kept per character
in memory. Shared-world `CompanyLevelUnlock` quest rewards (Donella's quests 80101045 and
20001072) call `unlock_position` after the quest completion commits and are re-applied at world
entry (see [quests.md](quests.md)). Quest `Company` conditions use `CompanyStatus`, including
`at_capped_level_ceiling`, which mirrors the client's capped-ceiling test.
Shared-world `CompanyContribution` quest rewards are credited to the current company after the
completion commits (at most once). Note that experience-based contribution only runs inside the
tutorial session today, and a character at the level cap earns no further experience, so quest
rewards are the open-world contribution source. Contribution decay is not modeled.

## Operator and GM control

- Operator console: `POST /admin/v1/characters/{id}/company` with `company_type` (0 removes
  the company), optional `position_level` (sets the threshold contribution and unlocks it) and
  optional `contribution`. A guild member must leave its guild before its company changes.
- GM commands (`GmCmdReq.company {type, add_contribution, pioneer_permit, position_lv,
  unlock_max_position}` or the text grammar in `social_gm_commands.hpp`): `company <type>
  [delta]`, `company_contribution <delta>`, `company_position <level>`,
  `company_unlock <level>` and `pioneer_permit <minutes>`.

## Validation and limits

`src/world/features/company/tests/company_tests.cpp` covers position levels, caps and unlocks, join/change rules,
experience conversion, permits and their charge, store failures, the PostgreSQL round trip,
catalog loading (synthetic and the exported catalog), dialogue resolution, packets, the GM
grammar and the lobby/selection/deletion views. `tools/native_social_smoke_test.py` drives the
operator seat, lobby company and a company-gated guild through a real server. These are
synthetic checks; the original client's company UI has not been observed with the emulator.
The atomic permit failure and retry tests were added during integration and have not yet run.
