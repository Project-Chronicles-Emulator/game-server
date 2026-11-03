# Project Chronicles Team capture corpus and IDA handoff

Verified on 2026-10-24 at 20:53 UTC. The human reports three new substantial
captures: another full tutorial and two open-world parts, reaching approximately
level 8 overall. That level and feature coverage are human-reported; confirm the
player's actual progression from decoded packets before using it as a protocol fact.

## Three new capture sets

Paths below are relative to the emulator repository. All three have a
`summary.json`, the byte-exact `native-session.bin`, and `decoded-analysis.jsonl`.
The binary SHA-256 values and decoded record counts were checked against each
summary. Keep the binary and decoded payloads private under ignored `out/`.

| Capture directory under `out/captures/` | Complete records | Coverage |
| --- | ---: | --- |
| `full-tutorial-capture-20261024-152100` | 4,077 | New full tutorial |
| `generic-part-1-through-level-6-20261024-153513` | 36,676 | Open-world part 1; level 6 reported by the human |
| `generic-part-2-20261024-162411` | 30,219 | Open-world continuation; the human now reports approximately level 8 overall |

Total: **70,972 complete records**. The captured native-log build identifier is
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa` in all three sets.

Verified `native-session.bin` SHA-256 values:

- Tutorial: `4d5c42e573212b230a23bb09d15f7a200e1b79043f1606a6d834cd1843ac2fbb`
- Part 1: `b97f0f55fef2048b2a4c091f029192dc4f4a4a7f756fd7ef7730a6adabce35c6`
- Part 2: `831b3466437b77f4ee7037f57b8f6ec8ce87dd8ee7be3cc829a5ed6fc70d58ea`

These supplement the older tutorial corpus. Search these new sets before repeating
an older finding that a request or behavior has never been observed, particularly
for inventory, equipment, item deletion, currencies, quests, NPCs and shops.
Retain packet direction, opcode, ordering, build identity and evidence locations.
Do not infer feature completeness from a capture label or elapsed play time.

## Additional activities capture — reviewed 2026-10-26

`out/captures/additional-features-20261026-012543` contains **16,671 complete
frames**, 118 packet types, 1,817 client frames and 14,854 server frames. Its build
identifier matches the original three. Canonical `native-session.bin` SHA-256:
`d8db8c1c6e5544fa2a4efe8ed5597935c8281a6ebc823292e18ac58679f7b357`.
Strict-tail replay decoded and round-tripped every complete frame with no errors
or unknown packets. The original three binary hashes and counts were rechecked.

The watcher exited on a Windows status-file replacement denial. The game log at
stop exactly extended the saved prefix, allowing recovery of all complete frames
through that snapshot. Eight incomplete trailing bytes remain in the private
source snapshot; the canonical replay contains complete frames only.

Compared with T/P1/P2, the new packet names are `PS2C_AttachDoodadNoti` (3),
`PS2C_DetachDoodadNoti` (3) and `PS2C_DoodadInteractionCountNoti` (5).
New observed behavior:

- Two own sit/stand cycles on seat slots 0 and 1, animation/interaction 70.
  The third attachment pair belongs to a nearby actor. Seating does not establish
  housing construction or land ownership.
- Two Smelting results for recipe 60203201 and three Carpentry results for
  recipe 60207101, extending the original nine Cooking results.
- Five interaction-49039 contributions consume item 49025 from quantity 5 to 0,
  with counters 1 through 5. The static phase threshold is 20; neither reaching
  that threshold nor the linked object/event continuation was observed.
- Interaction IDs 71 and 72 execute ChannelMove tasks, each followed by successful
  map-start/completion notifications on the same map 9012. Specific channel numbers
  and instance policy remain unresolved.
- Additional gathering animations 1010 and 1031, plus generic Use animation 11150
  whose player-facing purpose is unresolved.

New completed quest IDs are 10000162 (previously accepted) and 90118004; one new
NPC CMS type is 1001674. There are no new map IDs or requested player/item skill IDs.
The complete four-capture corpus now contains **87,643 frames across 156 packet
types**, eight maps, 44 completed quest IDs, 174 NPC CMS types and 40 requested
player/item skills. No new chat, full party lifecycle, housing construction,
guild, mount, mail, market or paid-shop flow was established.

Sanitized per-capture counts, static identifiers and comparison boundaries live in
`protocol/demo-capture-observations.json`. The generated feature reference groups
them by feature and retains the separate runtime implementation labels.

## Mage tutorial and mob combat capture — reviewed 2026-10-26

`out/captures/extra-capture-mage-data-20261026-022641` is labeled
**Extra Capture - Mage Data**. It contains 8,449 complete frames and 124 packet
types. Canonical SHA-256:
`0c1d8324956a348cfded6061789fc4d202f090a4cd226f53195a635532f1ae81`.
The build matches the previous four. The recorder stopped normally; its original
segment retains 182 incomplete trailing bytes, excluded from the canonical replay.
All complete frames decoded and passed semantic round trips. Both base snapshots
identify Magic; observed levels are 1–3.

Compared with T/P1/P2/A1, M1 adds 22 requested ElementalStaff skill IDs and 22
NPC/skill associations, including 16 skill IDs absent from prior NPC casts.
There are 471 own skill-start requests, 469 own start notifications, 145 mapped
NPC casts across 16 caster types, and 59 separate nearby-player starts. Two own
starts (2249/2203) receive `Skill_InReaction`. The only new packet name is
`PC2S_CharExitReq`; combat discoveries use known message formats.

The new evidence includes NPC melee combinations, backward evasion, shield guard,
the Warden's previously unobserved attack variants, mage fire/lightning charge
variants, barrage projectiles and ground-landing input. Evasion 190700800 is
cataloged as `Normal` but is not established as a damaging attack. Guard 194100100
also must not be treated as ordinary attack damage.

Forty-seven direct NPC hits on the player have matching active-map caster, cast
instance and explicit skill identity. The Infantryman's 190100300 has six hits
at a median 1,397.776 ms after start. The Prowler's 210130100→200 and 200→300
transitions recur near 1,698 and 1,447 ms, contradicting a universal 3,000 ms
cast interval. These are measured encounter intervals, not recovered AI rules.

All 23 own stage inputs are correlated, including inputs preceding the server's
start notification. The 369 client effect/projectile-spawn reports form 41
skill/layer/stage/unique-event groups. The old 2249 stage-0 timing attribution
conflicts with the catalog and the new stage-1 observations; review it before
rebuilding runtime profiles. This capture analysis leaves those profiles intact.

Sanitized tables, sample sizes, ranges, clock domains and limitations are in the
**Mage combat capture comparison** section of `docs/protocol-feature-reference.md`
and its owning `protocol/demo-capture-observations.json` input. NPC membership
extraction was independently cross-checked against the existing capture importer.
No new maps, NPC types or status-effect types appear. New quest completion
90104102 raises the corpus total to 45 completed quest IDs.

The five-capture corpus now has **96,092 frames across 157 packet types**,
13,338 client frames, 82,754 server frames, eight maps, 174 NPC CMS types and
62 requested player/item skills. New observation does not imply runtime support
or original-client acceptance of the emulator.

## Wavesword and open-world combat capture — reviewed 2026-10-26

`out/captures/wavesword-mob-combat-20261026-031521` contains **3,953 complete
frames across 72 packet types** (750 client, 3,203 server). Its canonical SHA-256
is `52b2f9cff85289089343e50a0e2eecbb19fcb538c2da78469657c33a8a545249`.
The build matches the preceding five. Strict replay decoded and round-tripped
every complete frame; the original segment preserves 125 incomplete trailing bytes.

The capture starts at source offset 954,414 in an existing log. Its private
`pre-start-context.bin` (SHA-256
`92ab906ff8cd48927ed19df1a6d568448a244bdd2e489ef696f184590e3ccccf`) separately
replays 12,199 earlier complete frames. It establishes the initial Magic class,
level 3, map 9012 and live actor identities. Context frames are excluded from W1
counts and novelty; cast-interval samples cannot cross this boundary.

W1 adds twelve own CurvedSword/Wavesword skill IDs, fifteen NPC/skill associations,
and two NPC CMS types (5001285 and 5001287). There are 183 own skill starts,
152 mapped NPC starts across 52 associations and nine caster types, and 21 nearby
player starts. All 19 direct NPC hits on the player have matching cast timing.
Eleven of twelve NPC/skill/effect timing groups had no prior matched direct-hit
sample on the recording player. Repeated Pallatius chains provide intervals near
1.79/1.99/1.26 seconds; they do not define general AI scheduling.

The sword data contains 342 effect reports, 163 matched effect-failure responses
and 129 own direct damage notifications. Contact-window reports and overlap-group
rules must remain distinct from successful hits. The one execution action has two
damage events, but does not establish general eligibility or interruption rules.

The only new packet name is `PS2C_HealNoti` (one). It belongs to a nearby LongSword
player's self-directed, damage-linked notification, not the recording character.
The static effect has healHpPerDamage=15; a positive restored amount is unproven.
New status 2000000 and projectile type 100200 also belong to nearby actors.

The six-capture corpus now contains **100,045 frames across 158 packet types**,
14,088 client and 85,957 server frames, eight maps, 45 completed quest IDs,
176 NPC CMS types and 74 requested player/item skill IDs. Sanitized comparison,
sample counts, timing ranges and implementation limits are in the generated
**Wavesword and open-world NPC combat comparison** section and its canonical
`protocol/demo-capture-observations.json` input. Runtime profiles are unchanged.

## Warrior tutorial capture — reviewed 2026-10-26

`out/captures/warrior-tutorial-20261026-034504` contains **4,948 complete
frames across 93 packet types**: 1,170 client and 3,778 server frames. SHA-256:
`f4b49485906ad0c2dec09ca2cf55b93f541fadb9defff577298d5c5c1d52f040`.
The build matches all six earlier captures, including Wavesword. Strict replay
decoded and round-tripped every complete frame. The original segment retains
170 incomplete trailing bytes, excluded from the canonical replay.

The recording starts at source offset 1,359,913. Its separately replayed context
has 17,061 complete earlier frames, SHA-256
`259eb0e1f12153416b4d1e7aa1fa9ba2026403a52e0f3cbbd65e78394cb59042`.
Context establishes the initial Fight class, level 1 and map 8000. It contributes
no frames, casts, timing samples or novelty to R1. R1 observes Fight levels 1–2.

R1 adds four NPC/skill associations: Skeleton Scout 5001010/190100400,
Skeleton Warder 5001016/190700500, Herald Knight 5001527/230240200, and
Recalling Grounds Warden 5001529/220210500. Three skill IDs are globally new
among NPC casts; 190100400 previously appeared on another NPC. Only the Warder
and Warden associations are absent from the reviewed runtime membership input.
There are 63 NPC starts across 42 associations and 12 caster types, separately
from 344 own starts and five nearby-player starts.

All 41 NPC-to-player damage notifications match in-slice casts: 36 positive
amounts and five zero contacts. The positives form 27 timing groups; 15 groups
(20 notifications) have no earlier matched positive hit on the recording player.
Scout flurry hits occur near 502/1,053/1,858 ms, Warder staff slam near 1,600 ms
and Warden charging slash near 896 ms. Herald Knight effect 230240210 has no
overlap group and lands four positive hits in one cast. Prowler chain intervals
near 1,704/1,450 ms reinforce Mage evidence; they are observations, not AI rules.

The 35 new own longsword/greatsword action IDs were already statically indexed.
R1 has 353 requests, nine reaction-locked refusals, 374 effect reports,
148 correlated ordinary effect-refusal responses and eleven landing inputs.
The sole new packet name is `PS2C_ResetCooldownNoti`, identifying effect 110328
alongside own parry counter 1127. Its self-targeted catalog cooldown reset names
pursuit skill 1000526; before/after readiness and general reset rules are unproven.

The seven-capture corpus contains **104,993 frames across 159 packet types**,
15,258 client and 89,735 server frames, eight maps, 45 completed quests,
176 NPC CMS types and 109 requested player/item skill IDs. The generated
**Warrior tutorial mob combat and weapon actions** section has eleven sanitized
tables, prior comparisons and explicit implementation limits. No runtime profiles
or behavior were changed by this capture analysis.

## IDA availability snapshot from 2026-10-24

The existing GUI database is reachable through `ida-multi-mcp`:

- Instance: `k7ui`, process `15412`.
- Binary: `D:/PROJETOS/Archeage Emulator/dumps/ArcheAgeChronicles.memdump.exe`.
- Architecture: x86-64.
- Binary SHA-256: `e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
- Two status snapshots returned `finished=true`, `queue_empty=true`, `state=idle`
  and an unchanged **705,939 functions**. The earlier auto-analysis blocker is cleared.

OMP agents already have the server configured in `.omp/mcp.json`. Use that
existing MCP connection and re-list instances/check status when starting new
research; instance IDs and process state can change. Do not reopen the database,
start another IDA copy, or enable OMP's native idalib against the GUI-held database.
`tools/agent_team/runtime.yml` intentionally disables that separate native tool.

The capture build identifier and dumped-binary SHA-256 are different identifiers;
their presence alone does not prove a matching-build relationship. Preserve and
verify provenance before treating a recovered address as evidence for a packet.

This handoff updates shared evidence and tool availability. It does not authorize
new implementation, builds, deployments, or resuming a stopped task by itself.
