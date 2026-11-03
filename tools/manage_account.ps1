#requires -Version 7.2

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Create', 'Verify', 'Role')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_]{3,32}$')]
    [string]$Account,

    # Required with -Action Role. A game master ("gm") gets the client GM console and may
    # run supported GM commands. The change applies at the account's next login.
    [ValidateSet('player', 'gm')]
    [string]$Role
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$credentialsPath = Join-Path $projectRoot 'out\local-postgres\credentials.local.json'
$accountTool = Join-Path $projectRoot 'out\build\windows\bin\Release\chronicles-account.exe'

if ($Action -eq 'Role' -and -not $Role) {
    throw 'Choose -Role player or -Role gm with -Action Role.'
}
if ($Action -ne 'Role' -and $Role) {
    throw '-Role is only valid with -Action Role.'
}
if (-not (Test-Path -LiteralPath $credentialsPath -PathType Leaf)) {
    throw 'The private PostgreSQL cluster has no credentials file. Start it first.'
}
if (-not (Test-Path -LiteralPath $accountTool -PathType Leaf)) {
    throw 'chronicles-account.exe is missing. Build the Release preset first.'
}

$credentials = Get-Content -LiteralPath $credentialsPath -Raw | ConvertFrom-Json
if ($credentials.version -ne 1 -or
    $credentials.database_password -notmatch '^[a-f0-9]{64}$') {
    throw 'The private PostgreSQL credentials file is invalid.'
}

$passwordPointer = [IntPtr]::Zero
$previousDatabaseUrl = $env:CHRONICLES_DATABASE_URL
$previousPgPassword = $env:PGPASSWORD

try {
    $env:PGPASSWORD = $credentials.database_password
    $env:CHRONICLES_DATABASE_URL =
        'host=127.0.0.1 port=5433 user=chronicles_local ' +
        'dbname=chronicles_local connect_timeout=5'

    if ($Action -eq 'Role') {
        # A role change needs database access only; no account password is read.
        & $accountTool 'role' $Account $Role
    } else {
        $securePassword = Read-Host "Password for $Account" -AsSecureString
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePassword)
        $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
        $password | & $accountTool $Action.ToLowerInvariant() $Account
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Account $Action failed (exit code $LASTEXITCODE)."
    }
} finally {
    $password = $null
    if ($passwordPointer -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer)
    }
    $env:CHRONICLES_DATABASE_URL = $previousDatabaseUrl
    $env:PGPASSWORD = $previousPgPassword
}
