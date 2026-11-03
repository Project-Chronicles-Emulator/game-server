# Client archive and catalog tooling

Run from the game-server root:

```powershell
pwsh -File tools/build_asset_extractor.ps1
```

Follow the [complete game-data guide](../../docs/guides/game-data.md) for local
prerequisites, archive extraction, descriptor recovery, catalog export and verification.

The archive reader is a development tool for the researched client build. It is not
linked into the C++ emulator runtime. CUE4Parse is fetched as pinned upstream source
under ignored `out/dependencies`; game files, keys and decoder binaries are supplied
locally. Use a fresh output directory outside the installation. Preserve upstream
dependency notices when distributing any separately built tool.
