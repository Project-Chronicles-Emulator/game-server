# Client catalog

From `Emulator/`, populate and verify the data folder:

```powershell
python tools/export_client_data.py --source 'G:\Dumps\Archeage\ClientData_2026-10-23' --include-raw
python tools/export_client_data.py --verify-only
```

The structured catalog is under `data/generated/catalog/`; omit `--include-raw`
when only that catalog is needed.

Add `--include-raw` to copy the full verified 11,720-file raw extraction into
`data/generated/raw/`. It is ignored by Git and checked against
`raw-export-summary.json` during `--verify-only`.

The server accepts `--client-data-dir data/generated/catalog`. JSONL tables
are in `Tables/` and preserve the original client field names and values.
See [client-data.md](../docs/client-data.md) for provenance, scope, and usage.
