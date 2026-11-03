# Maintained game-server tools

Use dev_shell.ps1 and the root CMake presets to compile. generate_protos.py handles
recovered schemas. The data generators, extraction helpers, import_protocol.py,
UMAP tooling and protocol reference generator remain with the game service.

install_database.ps1 installs the single complete schema. package_backend.ps1
packages game executables. start_distribution_backend.ps1 can start a configured
backend after the separate login authority is running.

The old combined memory/persistent startup wrappers and combined desktop release
packager were retired during service separation. Client preparation and desktop
packaging belong to the launcher repository; operator proxy tooling belongs to admin.
Historical documentation may mention retired commands. Current installation follows
README.md and config/services/game.example.json.
