# BinaryConfig offline format findings

Inspected read-only on 2026-10-23:

- Input: `out/native-config/raw/X5/Config/BinaryConfig.ini`
- Size: 1,715,920 bytes
- SHA-256: `6b5f0126886393cde82e34031683675e8408531de3165087d1362a8e39f0f7e5`

No game files, configuration files, archives, or runtime state were changed.

## Established byte structure

The first two little-endian 32-bit values are 0 and 1. At offset `0x8`, a
254-section configuration map begins. Each section contains:

1. A signed 32-bit string length followed by its string data. Positive counts
   use one-byte characters; negative counts use UTF-16LE code units. Nonempty
   strings in this region include their null terminator in the count.
2. A 32-bit key/value pair count and ordered pairs of strings.
3. An additional count and string pairs. Its semantic role remains tentative;
   the only nonempty instance here maps `CVarsArray` to `Name` in
   `/Script/Engine.DataDrivenConsoleVariableSettings`.

Repeated configuration keys occur and must retain their order. Converting these
entries to an ordinary dictionary would lose information.

The first map ends at `0x55f0e`. Subsequent data includes flags, `Engine` and
`Windows` strings, and file/hierarchy information. Some following filename
records use a length that excludes a terminator, so the section-string reader
cannot simply be applied to the entire remaining file.

## Read-only roundtrip check

An in-memory parser decoded the first section map, retained string representation
and pair ordering, and serialized that decoded structure again:

| Check | Result |
| --- | ---: |
| Parsed region | `0x8` through `0x55f0e`, end exclusive |
| Parsed bytes | 352,006 |
| Sections | 254 |
| Ordered key/value pairs | 3,444 |
| Decoded strings, including names and extra pairs | 7,144 |
| Re-encoded region equals original bytes | Passed |
| Remaining unparsed bytes | 1,363,906 |

This establishes a lossless roundtrip for that region. It does **not** establish
a whole-file parser, writer, or a loadable replacement archive.

## Bootstrap endpoint search

Case-insensitive searches in both one-byte and UTF-16LE representations found
zero occurrences of `BootstrapUrl`, `Bootstrap`, or `ArcheAgeChronicles`.
`DefaultX5Game.ini` occurs four times as one-byte strings at `0x106371`,
`0x10c37c`, `0x11109d`, and `0x1a2c66`; their full owning records remain unparsed.

The bootstrap endpoint therefore must not be assumed to exist in this extracted
binary cache. The runtime custom configuration entry can instead originate from
a later load of the packaged text INI.

## Existing utilities and related static evidence

Workspace searches found no existing binary-config cache parser. The available
`UE4Config.dll` exposes text INI parsing and hierarchy/cache helpers, but no
matching serialized `BinaryConfig` reader/writer was identified in its public
API.

The auth investigation independently verified in IDA that the BinaryConfig
loader and `IniBootstrap` wrapper share deserializer `0x141971430`. The wrapper
`0x141971970` then reads fourteen global INI-name strings. In that shared layout,
the first zero is the dynamic-file-map count; the following data includes ten
core config branches. Cached pseudocode is under
`../../dumps/config_override_research/`.

A candidate `IniBootstrap` archive must preserve the complete shared cache
serialization and supply the required appended names and compatible archive
flags. The partial roundtrip above is insufficient evidence for constructing or
installing such an archive.

## Candidate splice, not installed or runtime verified

`tools/binary_config_codec.py` now creates an optional candidate under `out/`.
It first verifies the unchanged map roundtrip and original input hash, appends
`SSL.DebuggingCertificatePath` to the **active Engine state**, then retains every
other ordered record and the entire 1,363,906-byte opaque suffix byte-for-byte.
The active-map identification is supported by the auth investigation's
`0x141951700` → `0x1419519F0` and lookup `0x14195D8D0` → branch `+1104` evidence.

The separate compact dynamic-branch layout comes from `0x141951890`, with the
following order: bool32, active config state, empty generic map, int32, two more
config states, FString-backed FName, FString. Each config state contains the
section map, three bool32 fields, FString-backed FName, FString, and a generic
map. Both source and IniBootstrap memory readers call `0x1419CFF20` to enable the
same archive flag, which omits the additional hierarchy metadata. The original
shared core bytes are therefore spliced opaquely after the injected entry.
These static findings and their IDA persistence belong to the auth investigation;
the offline tool does not read or mutate process memory.

The generated candidate is:

- `out/native-config/IniBootstrap.local-v2.bin`
- SHA-256: `87666b3df86393ef9d62326430f672e7adfb58103e1254b2d02b6dbeef39ad36`
- Size: 1,720,980 bytes
- Detailed checks: `IniBootstrap.local-v2.bin.evidence.json`
- Explicit global names: `ini-bootstrap-names.candidate.json`
- Injected prefix: 4,670 bytes, including one dynamic entry
- Dynamic key: `../../../X5/Config/DefaultX5Game.ini`
- Dynamic active state: all nine extracted custom INI sections, retaining
  duplicates and ordering; only `Server.BootstrapUrl` changes to
  `http://127.0.0.1:8080/`
- Engine change: one new SSL section and one certificate-path pair
- Shared core payload after that SSL change: 1,716,036 bytes, copied exactly
- Original opaque suffix SHA-256:
  `7d63f12d662c2e8ceb577ea228be04647a083fea1eafe6de711b2c376cd75dcd`

The fourteen trailing values use five canonical zero-length FStrings, the full
generated Compat and Lightmass destination paths under the previously observed
`C:/Users/Administrador/AppData/Local/X5/Saved/Config/Windows/` directory, then
`Scalability`, `Hardware`, `Input`, `Game`, `GameUserSettings`, `RuntimeOptions`,
and `Engine`. The auth investigation derived the two full paths from verified
path construction and matching same-build dump string lengths; they were not
read from a currently running process.

Remaining candidate assumptions are explicit: new branch/config-state flags
are zero; generic metadata maps are empty; the branch integer is zero; unused
FName metadata is `None`; state filename metadata is empty; and the branch
filename metadata repeats the supplied dynamic key. The field order and widths
are supported by static evidence, but the selected values and complete archive
acceptance still require an original-client trial. No game files were changed.

Reproduction from the workspace root, using a **new** output filename:

```powershell
python Emulator/tools/binary_config_codec.py `
    --output out/native-config/IniBootstrap.local-v2.bin `
    --value 'D:/PROJETOS/Archeage Emulator/Emulator/out/native-server/localhost.pem' `
    --dynamic-ini Emulator/out/native-config/raw/X5/Config/DefaultX5Game.ini `
    --dynamic-filename '../../../X5/Config/DefaultX5Game.ini' `
    --global-ini-names Emulator/out/native-config/ini-bootstrap-names.candidate.json `
    --bootstrap-url 'http://127.0.0.1:8080/'
```

The tool refuses to overwrite its source, existing candidates, or reports. It
accepts only the verified source hash, bounds strings/maps/file sizes, and rejects
unsupported custom INI operators rather than guessing their behavior. Existing
raw string encodings, duplicate keys, extra pairs, and original section order
are preserved.

Validation: eight focused Python tests passed, covering string bounds/encodings,
duplicate preservation, SSL addition, dynamic splice boundaries, canonical
empty trailing names, INI quoting and clear-array behavior, malformed input,
and native bootstrap endpoint composition. The tool checks that direct suffix
concatenation yields exactly the path `/api/auth/BootstrapReq` and records the
full resulting URL in its evidence report.
Both tool files also passed `py_compile`. These are offline synthetic checks.

The earlier `IniBootstrap.local-candidate.bin` is superseded and must not be
used: its URL ended in `/native/bootstrap`, whereas the original getter appends
`api/auth/BootstrapReq` directly. The corrected candidate uses the base URL
with a trailing slash. This endpoint composition was independently checked by
the auth investigation before any client trial.

## V2 runtime failure and graphics comparison

The original-client V2 trial failed before login with an unsupported D3D11 RHI
error. V2 is therefore **not a working bootstrap archive**. Its original-byte
preservation checks alone do not establish a correct IniBootstrap boundary.

The bounded follow-up report is
`out/native-config/IniBootstrap.local-v2.graphics-comparison.json`. It confirms:

- All 254 original active Engine sections are byte-identical, including key
  ordering and raw string encodings. Only SSL is appended.
- `/Script/WindowsTargetPlatform.WindowsTargetSettings` retains
  `DefaultGraphicsRHI=DefaultGraphicsRHI_DX12` and
  `D3D12TargetedShaderFormats=PCD3D_SM6`. Their signed string counts and encoded
  bytes are identical. `TargetedRHIs` is absent from that Windows section in
  both inputs.
- Seven selected graphics/platform sections match completely, and all
  1,363,906 original bytes after the first active map match. The original
  Engine branch leading bool32 remains `1`.

This excludes loss or alteration of those graphics settings from the candidate
bytes. It does not establish which global filename or branch the running RHI
lookup selected.

The auth investigation identified an additional serializer in the BinaryConfig
loader **after** the shared cache deserializer. Therefore, retaining the entire
source through EOF before appending fourteen globals may retain an unrelated
BinaryConfig trailer where IniBootstrap expects its globals. This boundary
requires verification before another candidate is generated.

Offline terminal bytes provide a bounded anchor: at `0x1a27c4`, a 32-bit value
of `38` precedes 38 contiguous FString paths beginning at `0x1a27c8` and ending
at `0x1a2ecc`. A final 32-bit zero ends the file. The first path is
`../../../Engine/Config/Windows/WindowsGame.ini`; the last is
`../../../X5/Config/Windows/WindowsScalability.ini`. These anchors alone do not
identify the complete trailer start and must not justify blind truncation.

## V3: corrected shared serializer boundary

The auth investigation subsequently proved the boundary from the serializer
call order. Shared serializer `0x141971430` reads the optional-present bool32
at `0x1a27c0`, then `0x14194D370` → `0x141943570` reads a signed count followed
by exactly one FString per entry. The 38 paths end at `0x1a2ecc`. The packaged
BinaryConfig loader then separately calls `0x14194C5B0` → `0x141942540` for a
map of FString keys to counted byte arrays. Its count at that boundary is zero,
so that BinaryConfig-only trailer consists of precisely the final four bytes.
The IniBootstrap wrapper does not read this map.

V2 incorrectly retained that map count before its fourteen global strings. A
raw read from the correct shared boundary consumes the zero as the first empty
global, shifts all remaining names, sets the final `GEngineIni` string to
`RuntimeOptions`, and leaves `Engine` unread. This explains a concrete path by
which intact DX12 settings can be looked up under the wrong branch. The runtime
cause still requires confirmation with a corrected trial.

The replacement candidate is:

- `out/native-config/IniBootstrap.local-v3.bin`
- Size: 1,720,976 bytes
- SHA-256: `6cd2dcd5fd4f2be183633dd037b6e999d27d6a2384f39f1725a55dffcb569cc5`
- `IniBootstrap.local-v3.bin.evidence.json`
- `IniBootstrap.local-v3.independent-byte-comparison.json`

V3 equals V2 with only the four-byte zero map count removed at V2 offset
1,720,702. All other candidate bytes, including graphics sections and metadata,
are identical. Reading fourteen global strings at that position now yields
the intended values, `GEngineIni=Engine`, and exact EOF consumption. No graphics
values or shader command-line arguments were changed.

The codec guards this operation with the exact source-build hash, optional-tail
flag/count/38-string walk, expected endpoint, first/last path anchors, typed
empty-trailer parsing, and complete equality of the patched optional tail. It
does not infer a boundary from an arbitrary trailing zero. Top-level suffix
identity checks describe the initial SSL-only patch; the nested
`removed_binary_config_only_trailer` evidence explicitly records the subsequent
four-byte removal during IniBootstrap assembly.

Ten focused tests and `py_compile` pass. The additional tests exercise typed
trailer bounds/truncation/EOF and reproduce the V2 shifted globals versus V3's
correct fourteen-string read. Reproduction uses the earlier command with output
`out/native-config/IniBootstrap.local-v3.bin`; existing files remain protected
from overwrite. V3 is prepared for review and has not been applied by this tool.
