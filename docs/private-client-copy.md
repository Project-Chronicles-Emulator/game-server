# Private local client copy

`tools/prepare_private_client.ps1` prepares a separate client tree for local experiments. It
never edits the Steam installation, launches the game, or changes network rules. It copies the
root bootstrap file, Engine, X5 binaries and config, plugins, and game Paks, excluding runtime
`.log` files. Each copied file is checked against the source with SHA-256. The completed root
contains `.local-client-copy.json` with the source path, original client executable hash, Pak
mode, and a per-file hash manifest. A failed preparation leaves an incomplete directory for
inspection; it does not erase it automatically.

Run a dry-run first from `Emulator/`:

```powershell
.\tools\prepare_private_client.ps1 -Destination 'E:\ArcheAgeChronicles-LocalTrial' -DryRun
```

The current installed game has six Pak files totaling 46,580,618,610 bytes. Its source disk is
writable. A directory junction or hard link would also permit writes to the installed Pak bytes,
so `Auto` selects a full private copy on this machine. Preparing that copy requires an explicit
large-copy switch:

```powershell
.\tools\prepare_private_client.ps1 `
    -Destination 'E:\ArcheAgeChronicles-LocalTrial' -ConfirmLargePakCopy
.\tools\prepare_private_client.ps1 `
    -Destination 'E:\ArcheAgeChronicles-LocalTrial' -Verify
```

The helper refuses an existing destination or a destination nested inside the source. Do not
place the private tree under the Steam-managed game directory. The copy can take substantial
time and space; the dry-run reports `BytesToCopy` before any files are written.

`-PakMode ReadOnlyLink` is available only when the source Paks reside on a disk Windows reports
as read-only. It creates a junction to that directory and verifies the target and read-only disk
state. `Auto` chooses this mode only when that condition holds; otherwise it copies the Paks.
`-PakMode Copy` always makes independent private Pak files. The helper does not change a disk's
read-only state or the Steam installation's file permissions.

For a quick validation of the helper without copying the installed game:

```powershell
.\tools\test_prepare_private_client.ps1
```

This test uses a tiny synthetic game tree under ignored `out/`, checks dry-run, copy, hash
verification, tamper detection, and rejection of writable Pak sharing, then removes only its own
temporary fixture. Preparing a private tree alone does not establish that a second original
client will authenticate. The experimental replacement module has since allowed two private
client processes to reach separate title screens. A later GUI trial launched them with separate
named-account grants; each completed the grant exchange and sent `CharListReq` to the local
server. Both then created separate characters, completed world TLS, and had their world
credentials accepted. Both game windows displayed a wooded world-start scene, but the server
skipped its tutorial map bootstrap for their class/map, so player presence and visual
synchronization remain unverified for the two-client trial. See
[xigncode-local.md](xigncode-local.md) and [launcher.md](launcher.md).

After installing the experimental shim and enabling the private firewall group, check a
bootstrap archive before launching:

```powershell
.\tools\launch_private_client.ps1 `
    -PrivateRoot 'E:\ArcheAgeChronicles-LocalTrial' `
    -BootstrapArchive 'D:\PROJETOS\Archeage Emulator\Emulator\out\launcher\IniBootstrap.local-...bin' `
    -CheckOnly
```

Omit `-CheckOnly` to start one additional private process. The tool checks the private-copy
marker, the installed shim state, and outbound firewall coverage. A strict named-account server
requires a different fresh launch-grant archive for each client. The GUI launcher can create
that archive when **Guarded private copy** is selected; its Play action then calls this helper.
