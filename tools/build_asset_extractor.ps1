#requires -Version 7.2

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$dependency = Join-Path $projectRoot 'out/dependencies/CUE4Parse'
$revision = '28f385e622468c4ab84997d596b46bb1c3adfcfa'

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $dependency)) {
    Invoke-Checked 'git' @('clone', '--depth', '1', '--filter=blob:none', '--no-checkout',
        'https://github.com/FabianFG/CUE4Parse.git', $dependency)
    Invoke-Checked 'git' @('-C', $dependency, 'fetch', '--depth', '1', 'origin', $revision)
    Invoke-Checked 'git' @('-C', $dependency, 'checkout', '--detach', $revision)
}

$actualRevision = & git -C $dependency rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actualRevision.Trim() -ne $revision) {
    throw 'The extractor requires the documented CUE4Parse revision. Use a separate matching checkout.'
}

$project = Join-Path $PSScriptRoot 'asset-extractor/ChroniclesAssetExtractor.csproj'
$versions = Join-Path $PSScriptRoot 'asset-extractor/dependency-versions.props'
Invoke-Checked 'dotnet' @('build', $project, '-c', 'Release',
    '-p:CUE4PARSE_SKIP_NATIVE=true', "-p:DirectoryPackagesPropsPath=$versions")

Write-Output 'Extractor built. Follow docs/guides/game-data.md for archive and catalog export.'
