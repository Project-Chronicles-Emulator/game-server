# Native client configuration override research

The analyzed demo binary positively supports the command-line option
`-textconfig`. In `UE_Config_Initialize_WithTextConfigOption` at `0x141965560`,
the branch at `0x141965ACA` checks this option before loading the binary config
cache. When the option is present, it skips that cache and initializes the normal
text INI configuration system.

This is relevant because the packaged client contains `X5/Config/BinaryConfig.ini`.
A loose `DefaultX5Game.ini` alone cannot be assumed to replace values already
loaded from that binary cache.

The next reversible client test is to preserve the original text configuration,
change only `[Server] BootstrapUrl`, and launch the normal game with `-textconfig`.
Text config initialization checks that the project has `DefaultEngine.ini`; the
other extracted original configuration files may need to be staged unchanged if
the packaged text defaults are unavailable at that early point.

This finding proves the flag exists in this executable. It does not yet prove
that a loose config takes precedence over a packaged text INI after the flag is
enabled. Runtime verification must inspect the loaded BootstrapUrl and requests
received by the local emulator.

## Other options

`UE_Config_GetDestinationIniFilename` at `0x141961500` builds the command-line
option `%sINI=` using a config category name, then reads a destination file name.
For example, a normal category called `Game` uses `GameINI=`. This is a category
file-name override. Its applicability to the game's explicit
`DefaultX5Game.ini` lookup has not been established.

The binary also supports `IniBootstrap=<archive>` in the text configuration path,
at `0x141965BB1`. It expects a serialized config-cache archive, not a text INI.
Do not point it at `DefaultX5Game.ini`.

A generic per-key `-ini:<Category>:[Section]:Key=Value` parser has not been
established in this shipping binary. String searches alone are insufficient to
claim that syntax works for this build.

Cached decompilation evidence is under `../../dumps/config_override_research/`.
Relevant functions and branch annotations are also recorded in the IDA database.

## Current-session endpoint redirect

For the live trial, the active authentication manager was located through the engine's game
instance. It owns the region array at offset `+24` (80-byte entries; auth-address FString at
entry `+16`), selected region at `+120`, and current auth-address copy at `+208`.
The selected region was `NA`.

Both the selected region's URL and current URL were changed to `http://127.0.0.1:8080` in
their existing allocations. Original UTF-16 bytes and FString headers were saved under ignored
`out/native-client/`, and both writes were read back. No executable code, TLS validation setting
or archive was modified. These addresses belong to that process and must never be reused after
restart. Restarting removes the in-memory redirect.

The native logout reset clears the current URL copy but retains the region array. It may free
the old current-URL allocation. A manual rollback must first validate the live pointer and
allocation; blindly restoring saved bytes after logout would be unsafe.

The emulator must reply with the client's selected region. Set `CHRONICLES_CLIENT_REGION=NA`
for this trial; `tools/start_local.ps1` uses that region by default. Other server launches retain
the `local` default unless overridden. Native login through this redirect still needs a live
client retry; synthetic integration alone does not establish it.
