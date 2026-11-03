# Consumables, weapon selection and public field travel

This repair follows the interaction/mount deployment `f1fb522`. Server and synthetic-client
checks are separate from original-client acceptance. It adds one character column; no
account, character, inventory, quest or companion state is reset.

## Empty consumables

Shared-world combat sessions previously lacked the inventory callbacks installed in tutorial
combat. Consequently, item-skill requests were not owned by that session and the client could
retain its predicted animation. Both session types now resolve the current inventory owner,
check positive committed quantity before accepting an item skill, and atomically spend one
stack unit and its quick-slot reservation before granting the effect. Failed persistence does
not mark the action's item spent. Fields without NPC populations still schedule item effects.

All mapped item skills use the same validation. Empty stacks produce StartSkillFailRes 13
with skill and instance identifiers; no StartSkillRes 53 or StartSkillNoti 14 is emitted.
Static recovery of the original client's opcode-13 handler confirms its predicted-action
reset path. This does not prove that no predicted frame is visible before a network refusal.
Draw-box ItemUseReq 20077 already checks owned quantity before any roll or reward; its
existing atomic commit/refusal behavior is preserved. Unsupported item types remain partial.

## Second weapon

The Item catalog assigns Weapon_1 to every weapon as its default equipment slot. Comparing
that value literally against a Weapon_2 request rejected the second slot. Weapons now accept
either weapon slot; class, level, durability, ownership and armor slot checks remain enforced.
Moving an equipped weapon clears its previous visual slot.

WeaponSlotActivationReq 20078 now validates the selected equipped weapon, life/action state
and revision, commits the selection and responds with WeaponSlotActivationNoti 172. Duplicate
selection is a no-op. Refusals return the current slot because static client analysis shows
that the slot field is applied even when the error is nonzero. Switching refreshes equipment
stats without healing spent HP/SP and refreshes combat's weapon identity. Player skill start
and refusal notifications carry the selected slot. Equipment/selection reach nearby observers
and later sight-entry snapshots. Lobby, inventory bootstrap and PlayerNub restore the selection.

Schema 34 adds `active_weapon_slot` (default 0, bounded to 0/1) to the existing revisioned
character build. Memory and PostgreSQL share the contract. The change is additive and older
server binaries can ignore the column. Official weapon-switch delay policy has not been
recovered; this server acknowledges eligible idle switches immediately. Original-client
weapon HUD, animation and both-weapon combat acceptance remain pending.

## Region boundaries

The earlier Cursor investigation and saved client analysis identify two separate mechanisms:
static inactive-zone restrictions, and a pending ZoneChannelMoveReq 20090 without a response.
The latter carries position/yaw, but no target map or trigger ID. Its callers include spatial
region changes that do not overlap a `channelSession` trigger box. The previous server route
could therefore not resolve those requests even though the destination map was supported.

`tools/build_zone_channels.py` exports the hash-checked ArcheWorld `zone.json` occupied-cell
masks and unique ungated public Field map associated with each zone. Current output contains
11 fields and 15,976 non-overlapping occupied cells, SHA-256
`2e7042664c9179dd5dc50566926f3bb6d58f0061f065d7c9dcef33254adf1538`.
Deploy `data/generated/triggers/ArcheWorld-zone-channels.json` beside the existing trigger
export. Startup loads channel routing independently of NPC population availability.

Existing unambiguous eligible trigger boxes keep precedence. Otherwise the reported position,
bounded to five metres from the authoritative point, resolves an occupied public-field cell.
The server preserves authoritative coordinates, yaw, vitals, statuses and cooldowns, and runs
175 -> 20085 -> 176 -> 20086 -> 183. This is a Candidate local channel-selection policy informed
by recovered spatial-zone callers, 5100-cm cell conversion and authored grids; the official
capture corpus still contains no 20090 request.

An unresolved idle crossing receives a same-map handoff, allowing normal client transition
cleanup instead of silence. The actual UI recovery requires original-client confirmation.
Busy/dead sessions, private GM encounters, gated instances and unresolved channel-volume
allocation remain outside the bounded travel route. Static inactive-zone client flags remain
a separate launcher compatibility mechanism: this change does not remove that patch or claim
unavailable maps, NPC populations or content are implemented.

## NPC voice investigation

The operator narrowed the report to nearby overhead speech; quest conversations and
cutscenes were not reported as silent.

The verified catalog has 5,183 caption rows: 4,520 have no voice reference, 73 reference a voice
with no audio event, and 590 reference an audio event. There are 4,256 Voice rows, of which
1,154 name an audio event. Therefore text appearing without sound is expected for many authored
lines. It does not by itself identify a server bug.

Static analysis follows NpcCaptionNoti 71 through catalog caption selection and the local
caption/voice playback path. The same line selects a voice catalog audio asset and NPC or
local pawn; no extra scene-audio packet occurs between text and voice in this path. This does
not verify installed sound banks, language, mixer settings or a particular reported silent line.
No arbitrary sound packets or substituted voices were added. An audible original-client trial
of a known voiced line is still required before claiming an audio repair.

## Evidence

The matching IDA database was saved with stored-section verification. Twenty-five inspected
functions have cached pseudocode, names/comments, extents, byte fingerprints and an append-only
journal entry under the parent workspace's `dumps/ida_research/gameplay-repairs-20261029/`.

Focused unit and TLS checks cover empty item refusal, the last consumed item, persistence
conflicts, Weapon_2 equip/activation/reconnect, trigger and spatial region handoffs, retained
stamina, malformed/ambiguous grids and same-map recovery. Final suite, PostgreSQL, deployment
and client results are recorded in the feature verification entries and private release receipt.

The private protocol Page previously rejected synchronization with `patch_document_size_exceeded`.
Its publication fingerprint remains at the last verified version until synchronization succeeds;
local reference regeneration does not claim that the Page is current.
