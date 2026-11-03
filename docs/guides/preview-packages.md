# Running the preview packages

Download the Windows x64 ZIP and its `.sha256` file from each repository's Releases
page. Extract each component into a separate directory. Keep its DLLs, web files,
configuration examples and protocol directory with the executable. Supply your own
compatible client; no client installation, extracted assets or keys are included.

Install the Microsoft Visual C++ x64 runtime for C++ applications. Launcher/admin
also need Microsoft Edge WebView2 Runtime. The asset extractor needs .NET 10; its
Python utilities need the packages in `requirements.txt`.

Before starting services, follow the [complete environment setup](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/getting-started.md)
for PostgreSQL, the database installer, private service key, operator token and local
world TLS certificates. Use absolute private-key paths in your environment and do
not put secrets in public examples. Login and game use the same database and service
key. Run commands from each extracted package's directory.

## 1. Login

```powershell
.\chronicles-login-server.exe --config config/services/login.example.json
```

Start login first. The example listens locally on public HTTP 8080 and private 8090.
Database installation uses `tools/install_database.ps1` and the included complete SQL
schema; follow the setup guide rather than installing a second database for game.

## 2. Game

Before testing gameplay, complete the [data pipeline](game-data.md). The catalog
provides character health, stamina and starting skills; the matching tutorial
profile provides tutorial/combat behavior. Starting without them is a transport
check only: a map can load with empty stats and unusable skills.

```powershell
.\chronicles-server.exe --config config/services/game.example.json `
    --protocol-dir protocol --agent-engine-data agent-engine `
    --client-data-dir data/generated/catalog `
    --world-bootstrap-diagnostic true --world-handshake-delay-ms 2000
```

Always supply `--protocol-dir protocol` when using the binary package. It overrides
the source checkout path compiled into the executable and loads the descriptors and
opcodes shipped beside it. The agent-engine path similarly points at the packaged
configuration; it does not enable an external model service by itself.

The example also needs the locally created world certificate/key at the configured
paths. Review your private configuration before starting it. The game API defaults
to 8082 and world TLS to 8081. Catalogs and profiles are separate private runtime
inputs: follow the [game data pipeline](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/game-data.md)
and add `--client-data-dir PATH/TO/catalog` as appropriate. A binary package does not
provide the game's content or populate every map.

The two diagnostic options select the researched local tutorial path. The delay
lets the guarded local world-entry helper observe the pending TLS connection.
Keep login, game HTTP and world TLS ports consistent when using custom ports.

## 3. Launcher

Use launcher preview.2 or newer for unsigned local service profiles. Prepare its
local bootstrap inputs from your own extraction and the world's **public** PEM
certificate (never its private key):

```powershell
python tools/prepare_local_client.py `
    --extraction C:/ProjectChronicles/private/client-extraction `
    --certificate C:/ProjectChronicles/game-server/out/certificates/world.pem `
    --client D:/Games/Chronicles
```

The extraction must contain `raw/X5/Config/BinaryConfig.ini` and
`DefaultX5Game.ini` from the researched build. Keep the generated `runtime/`
private. The command does not modify the client. For an already prepared compatible
private client, add `--client-mode private_copy`; this does not prepare or patch
a stock client. Unknown builds fail the existing identity checks.

```powershell
.\chronicles-launcher.exe
```

The package contains a local `launcher-config.json`. Configure your own client path,
keep automatic service startup off for the split services and connect to login on
`http://127.0.0.1:8080`. Keep existing private settings when updating a package.
Sign in, start the client, select the realm and character using the displayed
keyboard shortcuts. Confirm health/stamina and a successful weapon action after
world entry. Numbered item shortcuts may be empty until items are assigned.
A loaded backdrop alone is not a successful gameplay test.

## 4. Admin

```powershell
.\chronicles-admin.exe --server http://127.0.0.1:8082 `
    --token-file C:/ProjectChronicles/private/admin-token.key
```

Enable the game operator API with the same private token. The admin token is distinct
from player credentials and the login/game service key.

## 5. SDK generator

Start `ArcheAgeSdkGenerator.exe` from its own writable folder. Follow the repository
README to select a supported local process/build and configure access. Generated
SDKs stay local. The Lua generator requires your own compatible local reflection
export; the package supplies no generated game SDK or process-memory bridge.

## 6. Asset extractor

```powershell
dotnet ChroniclesAssetExtractor.dll --help
python -m pip install -r requirements.txt
```

Follow the extractor README for your own archive directory, private key file, local
compatible Oodle decoder and a new extraction directory outside the client. No key,
decoder or game archive is distributed. The Python catalog/protocol tools run against
your own extracted snapshot and matching local descriptor-bearing executable/dump.

These packages are experimental previews for the researched build. Building or
starting a service does not establish complete gameplay or compatibility with another
client build.
