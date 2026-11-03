#requires -Version 7.2

param(
    [Parameter(Mandatory)]
    [string]$Config,
    [string]$Executable,
    [switch]$UseLocalDatabase
)

$ErrorActionPreference = 'Stop'
$emulatorRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configPath = (Resolve-Path -LiteralPath $Config).Path
$settings = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $emulatorRoot 'out/build/windows/bin/Release/chronicles-server.exe'
}
$serverPath = (Resolve-Path -LiteralPath $Executable).Path
$listeners = @(Get-NetTCPConnection -LocalPort $settings.port -State Listen -ErrorAction SilentlyContinue)
if ($listeners.Count -gt 0) {
    throw "Configured API port $($settings.port) is already in use. No process was stopped."
}
if ($settings.world_port) {
    $worldListeners = @(Get-NetTCPConnection -LocalPort $settings.world_port -State Listen -ErrorAction SilentlyContinue)
    if ($worldListeners.Count -gt 0) {
        throw "Configured world port $($settings.world_port) is already in use. No process was stopped."
    }
}

$databaseUrl = $env:CHRONICLES_DATABASE_URL
if ($UseLocalDatabase -and [string]::IsNullOrWhiteSpace($databaseUrl)) {
    $credentialsPath = Join-Path $emulatorRoot 'out/local-postgres/credentials.local.json'
    $credentials = Get-Content -LiteralPath $credentialsPath -Raw | ConvertFrom-Json
    if ($credentials.database_password -notmatch '^[a-f0-9]{64}$') {
        throw 'The existing local database credential file is invalid.'
    }
    $databaseUrl = 'host=127.0.0.1 port=5433 dbname=chronicles_local user=chronicles_local connect_timeout=5 password=' + $credentials.database_password
}
if ($settings.storage -eq 'postgres' -and [string]::IsNullOrWhiteSpace($databaseUrl)) {
    throw 'Set CHRONICLES_DATABASE_URL or use -UseLocalDatabase with the existing private cluster.'
}

$runDirectory = Split-Path -Parent $configPath
$stdoutPath = Join-Path $runDirectory 'backend.stdout.log'
$stderrPath = Join-Path $runDirectory 'backend.stderr.log'
$previousDatabaseUrl = $env:CHRONICLES_DATABASE_URL
try {
    $env:CHRONICLES_DATABASE_URL = $databaseUrl
    $process = Start-Process -FilePath $serverPath -ArgumentList @('--config', ('"' + $configPath + '"')) `
        -WorkingDirectory $emulatorRoot -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
} finally {
    $env:CHRONICLES_DATABASE_URL = $previousDatabaseUrl
}

$deadline = [DateTime]::UtcNow.AddSeconds(20)
do {
    $process.Refresh()
    if ($process.HasExited) {
        throw "The backend exited with code $($process.ExitCode). Inspect $stderrPath and $stdoutPath."
    }
    try {
        $health = Invoke-RestMethod -Uri ($settings.public_address.TrimEnd('/') + '/health') -TimeoutSec 2
        if ($health.status -eq 'ok') {
            Write-Output "Distribution backend PID: $($process.Id)"
            Write-Output "API: $($settings.public_address)"
            Write-Output "Config: $configPath"
            return
        }
    } catch {
        # Startup is bounded by the deadline; a TLS trust/config failure is reported below.
    }
    Start-Sleep -Milliseconds 150
} while ([DateTime]::UtcNow -lt $deadline)

throw "The process started but its configured API did not become healthy. Inspect $stdoutPath."
