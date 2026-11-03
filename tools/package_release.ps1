#requires -Version 7.2
param([string]$BuildDirectory = 'out/build/windows')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$component = 'game-server'
$package = Join-Path $root "out/release/$component-win64"
if (Test-Path -LiteralPath $package) {
    throw "Package directory already exists: $package. Use a fresh output directory."
}
New-Item -ItemType Directory -Path $package -Force | Out-Null

function Copy-Required {
    param([string]$Source, [string]$Destination = $package)

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Required release input is missing: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

& cmake --install $BuildDirectory --config Release --component Backend --prefix $package
if ($LASTEXITCODE -ne 0) {
    throw 'Game server installation failed.'
}
New-Item -ItemType Directory -Path "$package/config/services" -Force | Out-Null
Copy-Required 'config/services/game.example.json' "$package/config/services"
Copy-Required 'protocol/packet-fields.json' "$package/protocol"
Copy-Required 'docs'

# Runtime generators must be usable from the binary package as well as a checkout.
# Select source files explicitly; extracted data and local keys are never packaged.
$generatorNames = @(
    'export_client_data.py', 'build_tutorial_data.py', 'build_combat_data.py',
    'build_gathering_data.py', 'build_housing_data.py', 'build_loot_data.py',
    'build_processing_data.py', 'build_adventure_objects.py', 'build_channel_event_data.py',
    'build_contents_guides.py', 'build_exploration_places.py', 'build_level_trigger_volumes.py',
    'build_map_static_npcs.py', 'build_map_travel.py', 'build_npc_reactions.py',
    'build_nui_statues.py', 'build_nuia_tears.py', 'build_quest_catalog.py',
    'build_stationary_npc_placements.py', 'build_world_mobs.py', 'build_zone_channels.py',
    'tutorial_interactions.py', 'tutorial_level_triggers.py', 'tutorial_quests.py',
    'tutorial_spawns.py', 'npc_patrol_data.py', 'world_layers.py', 'protocol_schema.py',
    'export_protocol_schema.py'
)
foreach ($name in $generatorNames) {
    Copy-Required "tools/$name" "$package/tools"
}
Copy-Required 'protocol/tutorial-combat-observations.json' "$package/protocol"
Copy-Required 'requirements-build.txt'

Copy-Required 'README.md'
if (Test-Path -LiteralPath 'LICENSE') {
    Copy-Required 'LICENSE'
}
# Package only explicitly selected binaries, configuration examples and tool source.
# Never copy the checkout, data/generated, private configuration or research exports.
$archive = "$package.zip"
Compress-Archive -Path "$package/*" -DestinationPath $archive
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
Write-Output $archive
