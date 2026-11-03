# Xigncode and local client startup

The original game executable loads Xigncode itself. Starting
`X5/Binaries/Win64/ArcheAgeChronicles.exe` directly, with a fresh emulator launch grant and
`-textconfig -IniBootstrap=...`, did not skip it. The observed process loaded
`XIGNCODE/x3_x64.xem` and spawned `XIGNCODE/xxd-0.xem`. The direct-launch client was stopped after
it opened an external IPv6 connection; that connection's purpose was not established. No
installed game file or Xigncode service was changed in this trial.

The matching IDA database identifies `AAC_Xigncode_LoadLibraryAndExports` at RVA `0x6536580`.
It loads `x3_x64.xem`, falls back to `xcorona_x64.xem`, and obtains interface exports. The caller
`AAC_Xigncode_InitializeRuntime` at RVA `0x65366d0` prepares the runtime and resolves interface
methods. `AAC_Xigncode_StartWithCallbacks` at RVA `0x6535540` invokes initialization and records
success or an error. Detailed pseudocode, fingerprints, and uncertainty are in
`../../dumps/ida_research/xigncode-local/FINDINGS.md` and the saved IDA database.

The installed game executable is packed: its large `.text` section has no raw on-disk bytes.
The RVAs above describe the unpacked memory image and cannot be used as offsets for a simple
file patch. No supported command-line opt-out was found in the analyzed startup wrappers.

The recovered front WebSocket schema also names `WSF2C_XigncodeQueryReq` (server opcode 25)
and `WSC2F_XigncodeQueryRes` (client opcode 20019). Both contain an `at` value and an opaque
byte buffer. The emulator does not send the query. The available official entry capture only
covers world traffic, so it cannot establish whether the original front service uses this
exchange during login. The response buffer cannot yet be synthesized from the schema alone.

## Diagnostic replacement trial

`tools/xigncode_shim_experiment/` contains a standalone C++20 DLL exporting the observed
ordinal resolver ABI and a guarded script that backs up, installs, and restores it only in a
marked private client copy. The first build stopped at a window titled `XignCode` before
loading. Static analysis of the game's startup loader found a PE section checksum marker in
the DOS header. The original module passed that check and the first build did not. After the
build began writing the computed marker, a contained client trial loaded the replacement,
opened an `ArcheAge Chronicles` window, and requested the local bootstrap endpoint. No
`xxd-0.xem` helper was observed. The game displayed its interactive title screen. A second
private client launched concurrently from the same copy and displayed the same screen; both
processes loaded the replacement module. Their no-grant development archive caused
`AuthReq -> 401` on the strict persistent server. Native login and world entry have not yet
been observed with the replacement.

## Private client network containment

`tools/private_client_firewall.ps1` creates outbound block rules for every `.exe` and `.xem`
inside a prepared private client root. It requires the `.local-client-copy.json` marker from
`tools/prepare_private_client.ps1` and refuses the installed game root. Run `-Action Enable`
before starting a private client, `-Action Status` to inspect the rule group, and `-Action Disable`
when the trial is over. The rules do not alter the installed game or its Xigncode service.

A temporary rule was tested with a copied `curl.exe`: HTTP to `127.0.0.1` succeeded, HTTPS to
an external address failed, and external HTTPS succeeded again after rule removal. The helper's
own enable/status/disable flow passed the same external-connectivity check using a fixture
private root. This verifies Windows Firewall behavior for the test executable on this machine;
it does not establish that every client child process is covered. Inspect the private process
tree and rule list before any local-only client trial.

`tools/launch_private_client.ps1` checks the private module, private-copy marker, and firewall
coverage, then starts another client with a specified bootstrap archive. It does not create an
account or grant. To use a named account, generate a separate fresh grant archive for each
launch; one grant cannot authenticate two clients. The world entry diagnostic now accepts
`--pid` and `--client-directory` so it can target one private process instead of selecting an
arbitrary running client.

**Status:** Two private client processes boot concurrently to the title screen without an
observed Xigncode helper. The emulator already supports separate account sessions and synthetic
two-client world presence/movement tests. Two original clients have not yet authenticated,
entered the world, or rendered one another with this replacement. Boot-time improvement has not
been measured. The GUI launcher now has a guarded private-copy launch target and creates a new
account grant per Play action. It still tracks only one private client for automatic world entry;
use the PID-targeted diagnostic for additional clients.
