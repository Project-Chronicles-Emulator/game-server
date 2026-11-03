# Run Project Chronicles locally

[← Documentation](../README.md) · [Components](components.md) · [Troubleshooting](troubleshooting.md)

This guide targets the current Windows x64 development presets. It first gets the
services and desktop tools running; the [data guide](game-data.md) prepares gameplay
content. A successful health check is not an original-client compatibility trial.

## 1. Install the tools

| Tool | Used for |
| :--- | :--- |
| Git | Clone the four source repositories |
| Visual Studio 2026, Desktop development with C++ and Windows SDK | MSVC compiler, linker and resource tools |
| CMake 3.25+ and Ninja | The checked-in Ninja Multi-Config presets |
| Python 3 and pip | Protocol generation, extraction and verification |
| vcpkg | The pinned C++ dependency manifests |
| PostgreSQL 16, including `psql` | Persistent accounts, characters and world state |
| PowerShell 7.2+ | Development and database helpers |
| WebView2 Runtime | Launcher/admin desktop interface |
| .NET 10 SDK and a local Oodle decoder | Only for initial archive extraction |

Install/bootstrap vcpkg and note its directory. The commands below assume
`C:\dev\vcpkg`; replace that value with your installation. The manifest resolves
Boost, OpenSSL, Protobuf, libpqxx, JSON, logging and test dependencies. Desktop
manifests additionally include WebView2. This is the documented Windows path;
Linux service packaging is not covered by these presets.

## 2. Clone the workspace

```powershell
New-Item -ItemType Directory -Path C:\ProjectChronicles -Force | Out-Null
Set-Location C:\ProjectChronicles
git clone https://github.com/Project-Chronicles-Emulator/game-server.git
git clone https://github.com/Project-Chronicles-Emulator/login-server.git
git clone https://github.com/Project-Chronicles-Emulator/launcher.git
git clone https://github.com/Project-Chronicles-Emulator/admin.git
```

The sibling layout is convenient for this guide, not a source dependency.

## 3. Build the four components

Run from the workspace above. Each `dev_shell.ps1` loads the MSVC environment;
dot-source it again when opening another terminal for CMake work.

```powershell
$workspace = (Get-Location).Path
$toolchain = 'C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake'
python -m pip install -r .\game-server\requirements-build.txt

Set-Location (Join-Path $workspace 'login-server')
. .\tools\dev_shell.ps1
cmake --preset windows "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
cmake --build --preset release --target chronicles-login-server chronicles-roster-tests

Set-Location (Join-Path $workspace 'game-server')
. .\tools\dev_shell.ps1
cmake --preset windows "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
cmake --build --preset release --target chronicles-server chronicles-roster-tests chronicles-services-front-tests

Set-Location (Join-Path $workspace 'launcher')
. .\tools\dev_shell.ps1
cmake --preset windows "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
cmake --build --preset release --target chronicles-launcher

Set-Location (Join-Path $workspace 'admin')
. .\tools\dev_shell.ps1
cmake --preset windows "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
cmake --build --preset release --target chronicles-admin
Set-Location $workspace
```

Check each command succeeds before continuing. Output locations:

| Component | Executable relative to its checkout |
| :--- | :--- |
| Login | `out/build/windows/bin/Release/chronicles-login-server.exe` |
| Game | `out/build/windows/bin/Release/chronicles-server.exe` |
| Launcher | `out/build/windows/bin/Release/launcher/chronicles-launcher.exe` |
| Admin | `out/build/windows/bin/Release/admin/chronicles-admin.exe` |

Keep each executable beside its generated/copied DLLs and web assets.

## 4. Install one shared database

Start PostgreSQL 16. From the workspace root:

```powershell
pwsh -File .\game-server\tools\install_database.ps1 `
    -PostgresBin 'C:\Program Files\PostgreSQL\16\bin'
```

The installer prompts for credentials, creates the dedicated application role/database
and applies **one complete schema**. Defaults are database/user `chronicles` on
`127.0.0.1:5432`. Use its host/port/name/user parameters when your installation differs.
Both services must use the same database. Run installation once, not one database per
service. Reapplying a complete current schema preserves data; older or unrelated
schemas are rejected rather than automatically upgraded. The inactive experimental
skill-item fixture is not part of the normal schema.

## 5. Create private service files once

From the workspace root, create files outside all four repositories:

```powershell
$workspace = (Get-Location).Path
$privateRoot = Join-Path $workspace 'private'
New-Item -ItemType Directory -Path $privateRoot -Force | Out-Null
foreach ($name in @('auth-service.key', 'admin-token.key')) {
    $path = Join-Path $privateRoot $name
    if (-not (Test-Path -LiteralPath $path)) {
        $bytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
        [IO.File]::WriteAllText($path, [Convert]::ToHexString($bytes).ToLowerInvariant())
    }
}
```

Restrict these files to the Windows account running your services. Login and game
share `auth-service.key`. The admin token is a separate operator credential; it is
not a player password or the service key. Preserve existing files when restarting.

## 6. Prepare world TLS

From the workspace root, use a local OpenSSL executable. Git for Windows commonly
provides the path below; change it if needed.

```powershell
$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'
New-Item -ItemType Directory -Path .\game-server\out\certificates -Force | Out-Null
& $openssl req -x509 -newkey rsa:2048 -nodes -days 30 `
    -subj '/CN=localhost' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' `
    -keyout .\game-server\out\certificates\world.key `
    -out .\game-server\out\certificates\world.pem
```

This certificate enables a local TLS transport test. Acceptance by the researched
game client is a separate, build-guarded concern; see [client limitations](components.md#launcher).
Replace it through your normal certificate process for an actual deployment.

## 7. Set each service terminal's environment

Open **two terminals at the workspace root**. Run this block in each, entering the
application database password selected during installation. Adjust database values
if you changed the installer defaults.

```powershell
$workspace = (Get-Location).Path
$privateRoot = Join-Path $workspace 'private'
$credential = Get-Credential -UserName chronicles -Message 'Project Chronicles database role'
$password = $credential.GetNetworkCredential().Password
$escapedPassword = $password.Replace('\', '\\').Replace("'", "\'")
$env:CHRONICLES_DATABASE_URL = "host=127.0.0.1 port=5432 dbname=chronicles user=chronicles connect_timeout=5 password='$escapedPassword'"
$env:CHRONICLES_AUTH_SERVICE_KEY_FILE = Join-Path $privateRoot 'auth-service.key'
Remove-Variable credential, password, escapedPassword
```

The servers read environment variables, not `.env` files automatically. Do not
paste database passwords into committed JSON, command arguments or issue reports.

## 8. Start login, then game

**Terminal A — login:**

```powershell
Set-Location (Join-Path $workspace 'login-server')
.\out\build\windows\bin\Release\chronicles-login-server.exe `
    --config config/services/login.example.json
```

**Terminal B — game:**

```powershell
Set-Location (Join-Path $workspace 'game-server')
.\out\build\windows\bin\Release\chronicles-server.exe `
    --config config/services/game.example.json `
    --admin-token-file (Join-Path $privateRoot 'admin-token.key')
```

The minimal configuration tests service operation without a full gameplay export.
After [preparing data](game-data.md), add
`--client-data-dir data/generated/catalog`. Generated feature profiles have their
own loader conditions; see that guide before expecting a populated world.

From another terminal:

```powershell
Invoke-RestMethod http://127.0.0.1:8080/health
Invoke-RestMethod http://127.0.0.1:8082/health
```

Login should become available first. The realm becomes available after game
heartbeats arrive and returns offline when they expire. Only one login and one
game owner may run against this database. Use Ctrl+C in their terminals to stop;
do not start replacements against a still-running owner.

## 9. Open the launcher and admin

From the workspace root, install the local launcher profile for a fresh build.
An existing profile is preserved:

```powershell
$profile = '.\launcher\out\build\windows\bin\Release\launcher\launcher-config.json'
if (-not (Test-Path -LiteralPath $profile)) {
    Copy-Item .\launcher\config\launcher.local.example.json $profile
}
.\launcher\out\build\windows\bin\Release\launcher\chronicles-launcher.exe
.\admin\out\build\windows\bin\Release\admin\chronicles-admin.exe `
    --server http://127.0.0.1:8082 `
    --token-file .\private\admin-token.key
```

Configure/select the launcher's **Local** server at `http://127.0.0.1:8080`.
Create a player account and sign in there. Choose your compatible local game
folder; there is no client download included with this checkout. Keep launcher
automatic local-server startup disabled: the services above are started separately.
The [component guide](components.md#launcher) includes an explicit local profile.

The admin uses the game endpoint and the operator token. It does not sign in with
the player account or connect to login's private authority port.

## What this proves

Healthy processes, a usable operator connection and a successful account flow
establish a development environment. They do not establish a full game playthrough.
Next: [extract data](game-data.md), [review progress](../progress.md), or
[choose a contribution](../../CONTRIBUTING.md).
