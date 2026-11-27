# Puts the files the game ships besides the program into its folder (beside "FrightWorks-No100F-Game.exe"): the button prompt
# icons in button_icons (tools\make-button-icons.py). They are program files: an installed game's folder is replaced on
# every update. No build ships an HD texture pack (2026-11-24): the launcher downloads one when the player chooses it,
# and an older build's staged copy (texture_replacements\ScoobyHD) is removed.
#   powershell -ExecutionPolicy Bypass -File tools\stage-assets.ps1 -GameDir <folder> [-Required]
# Used by build-no100f.ps1 (Play\, after every build) and package-scooby.ps1 (the release's data\). Only what changed
# is copied or rebuilt. -Required (packaging) stops on anything missing; without it a missing piece is a warning, so a
# test build still finishes.
# The work is done by stage-assets.py (Python 3, standard library only; on Linux run that directly).
[CmdletBinding()] param([Parameter(Mandatory)][string]$GameDir, [switch]$Required)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'invoke-python.ps1')
$arguments = @('--game-dir', (Resolve-PathArgument $GameDir))
if ($Required) { $arguments += '--required' }
Invoke-PythonScript (Join-Path $PSScriptRoot 'stage-assets.py') $arguments
