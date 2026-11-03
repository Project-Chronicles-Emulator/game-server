# Contextual contents guides

The matching client build contains `ContentsGuide` and `ContentsGuideCondition` tables and
localized text and key icons. Static client analysis identifies `PS2C_ContentsGuideNoti` (105):
its guide ID selects a client catalog entry and `start_trigger` chooses the start or stop path.
The client guide manager also installs the selected guide's own condition callbacks. The exact
screen layout and live-client behavior have not been observed in this implementation.

Generate the profile from the verified catalog and raw map layers:

```powershell
python tools/build_contents_guides.py
```

The normal full client export also builds `data/generated/contents-guides/definitions.json`.
The runtime loads that file beside `catalog/`; if absent, it logs a warning and leaves guides
disabled. The current export contains 193 text or control-tip guides on 96 maps, including the
two tutorial maps. Full-screen `Contents` cards are excluded from this side-prompt feature.

The server evaluates supported authored alternatives against its current character state and
position: trigger-volume entry and exit, quest requirements, owned item and grade, active weapon
type, successful instant interactions, and accepted server-owned cutscene endings. It sends a
start or stop only after the corresponding state changes. On first world entry it starts relevant
location prompts but does not replay every previously completed quest or owned item. Position
comes from the server's tracked movement; the profile uses the authored map-layer geometry.
Quest and item state is refreshed after committed events, without a database read on each move.

Unsupported conditions remain unknown, including when negated. An AND group cannot match
unless every condition is known and true; an OR group can match when a known condition is true.
Conditions such as skill checks, enemy reactions, swimming and mount state remain
unimplemented server-side. The client may still manage its own condition callbacks after a
guide starts; no claim is made that every one of the catalog's guides can be displayed by this
server. One-shot suppression is session-local, and it is not a durable per-character guide
history. Animated interactions without a matching instant interaction response are not yet used
as interaction triggers. See the protocol reference for the evidence and verification limits.
