# Nuia Tears (Tears of Nui)

The matching demo catalog calls the floating collectible `TearsOfNui`. It awards
stackable item **2000**, rather than Gold, Blue Salt or Hersander Tokens.

`tools/build_nuia_tears.py` joins `Map.runtimeDataLayerPath`, authored
`spot_object.json` placements, `AdventureObject`, `ExpertiseReward`, `LootPack`,
`LootCondition` and `LootItem`. The verified export contains **137 map/spot
placements**, **119 distinct object CMS IDs**, and **30 map IDs**, including
44 placements on the hosted map 9012. Repeated map appearances share the same
character-owned CMS completion marker. The catalog's deterministic quantities
are 1, 5 and 10; unsupported conditions or reward shapes fail generation.

The export uses the established build identity, source hashes, sector/local spot
ID layout and centimetre-to-millimetre conversion. Missing raw layers do not
produce placements. These counts describe the available matching-build export,
not every collectible in every game version or 30 running map services.

## Collection and persistence

`NuiaTears` is an immutable startup index. The native world session routes
`PC2S_ActivateAdventureObjectReq` (20049) to it on the existing session worker.
The request's `spot_object_id` resolves only on the server's current map.
Collection requires a living character, a tracked position within 10 metres
of the authored placement, and request coordinates within the existing
5-metre movement tolerance of that tracked position. These are bounded local
policies; they do not establish the official collection radius or authoritative
collision/speed simulation. Requests are limited to 32 per second per session.

On first collection the server commits the character's activated marker and
item-2000 balance in the **same existing world-progress revision transaction**.
Only afterward does it send:

1. `PS2C_ActivateAdventureObjectRes` (91), with the object CMS ID and Activated state.
2. `PS2C_ItemsUpdateNoti` (169), with `adventure_object_activate` and the resulting
   total item-2000 quantity.

A duplicate collection returns the activated state without another reward or
revision. A competing stale revision cannot commit, and the connection closes
without publishing the planned success. Other characters keep their own tears.
No new migration or global respawn timer is needed.

Discovery (20050) records Discovered without granting anything, and cannot lower
an Activated state. Shared-world time-sync sends saved adventure states through
`WorldCharAdventureObjectsNoti` (127); the existing inventory snapshot restores
the owned tear total. Tutorial snapshots already include these saved states.
The normal world inventory path also reevaluates carried weight after collection.

Automatic discovery is routed here only for an indexed tear on the tracked map.
Other well-formed adventure-object discoveries fall through without an error,
progress access or use of the tear request budget. Malformed discovery and
explicit activation retain their existing validation. See the
[2026-10-29 proximity repair](proximity-discovery-repair-20261029.md) for the
original-client failure evidence and regression checks. This does not implement
unindexed adventure-object types.

## Evidence and limits

The official P1/P2 captures contain **19 activation requests** (9 and 10).
All 19 spot IDs and returned CMS IDs match exported placements and are followed
by an adventure-object item update containing item 2000. Their request positions
are approximately 1,026–8,001 mm from the authored points. This supports the
proximity request and response/item-update shape, rather than an interaction key
or animation path. The catalogue supplies the 5/10 reward tiers separately.

Those captures also send `CharPlayRecordUpdateNoti`. Generic play-record counters,
collection achievements and related unlocks are not implemented by this change.
Other adventure-object types and exploration REST routes remain outside its scope.
Original-client acceptance against the updated local server and deployment remain
separate from the synthetic checks below.

## Generate and validate

From `Emulator/`:

```powershell
python tools/build_nuia_tears.py
python tools/test_build_nuia_tears.py
cmake --build out/build-chest --config Release --target chronicles-tests --parallel 2
ctest --preset release -R 'NuiaTears.*|NativeWorld.NuiaTearsCollectOncePerCharacterAndRestoreOverTls'
```

The standard client export and persistent startup helper regenerate
`data/generated/nuia-tears/definitions.json`. The world listener loads that file
beside the configured client catalog. An older installation without it logs the
missing path and leaves this feature disabled. The live service must use the
updated runtime and generated file through its established maintainer workflow.

Python checks cover the real catalog joins and all 19 official collection
associations when the private export/captures are available. C++ checks exercise
packet encoding, reward tiers, discovery, duplicate/reconnect/cross-map behavior,
independent characters, invalid targets and positions, overflow, malformed input,
and the existing revisioned storage transaction. The TLS regression exercises
the actual dispatcher and reconnect snapshots with two synthetic clients.

On 2026-10-26 the shared working tree built `chronicles-tests` and
`chronicles-server` in Release through `out/build-chest`. All **nine focused
CTest cases passed without skips**, including the two-character TLS/reconnect
regression. All **three Python extraction/capture tests passed**. The TLS test
also caught and verified the correction to the world dispatcher's position
gate: Nuia Tear collection works without a tutorial, quest, gathering or
processing service. Evidence is under ignored `out/nuia-tears-validation/`.
This is scoped synthetic validation of a candidate binary; the broader suite,
live deployment and original-client acceptance were not completed by this task.
