param(
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$ServerExecutable
)

$ErrorActionPreference = 'Stop'
$emulatorRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $emulatorRoot 'out/build/windows'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $emulatorRoot 'out/distribution'
}

$buildRoot = [IO.Path]::GetFullPath($BuildDirectory)
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$serverOverride = $null
if (-not [string]::IsNullOrWhiteSpace($ServerExecutable)) {
    $serverOverride = [IO.Path]::GetFullPath($ServerExecutable)
    if (-not (Test-Path -LiteralPath $serverOverride -PathType Leaf) -or
        [IO.Path]::GetFileName($serverOverride) -ne 'chronicles-server.exe') {
        throw 'The server override must name a built chronicles-server.exe.'
    }
}
$packageName = 'chronicles-backend-win64-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$packageRoot = Join-Path $outputRoot $packageName
if (Test-Path -LiteralPath $packageRoot) {
    throw "Package destination already exists: $packageRoot"
}

New-Item -ItemType Directory -Path $packageRoot | Out-Null
& cmake --install $buildRoot --config Release --component Backend --prefix $packageRoot
if ($LASTEXITCODE -ne 0) {
    throw "Backend package installation failed with exit code $LASTEXITCODE"
}

if ($serverOverride) {
    # Package an isolated server link while preserving the executable used by a live trial.
    $packagedServer = Join-Path $packageRoot 'chronicles-server.exe'
    Copy-Item -LiteralPath $serverOverride -Destination $packagedServer -Force
    if ((Get-FileHash -LiteralPath $packagedServer -Algorithm SHA256).Hash -ne
        (Get-FileHash -LiteralPath $serverOverride -Algorithm SHA256).Hash) {
        throw 'The packaged server does not match the selected executable.'
    }
}

foreach ($filename in @('chronicles-server.exe', 'chronicles-account.exe', 'chronicles-news.exe', 'chronicles-content.exe', 'README.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $filename) -PathType Leaf)) {
        throw "Backend package is missing $filename"
    }
}

# Verify relocation without DLLs supplied by the development machine's PATH.
$previousPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    & (Join-Path $packageRoot 'chronicles-server.exe') --help | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "The packaged backend cannot start independently (exit code $LASTEXITCODE)."
    }
} finally {
    $env:PATH = $previousPath
}

$archive = $packageRoot + '.zip'
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive
Write-Output "Backend directory: $packageRoot"
Write-Output "Backend archive: $archive"
Write-Output "SHA256: $((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash)"
