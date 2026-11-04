# Repeatable i001 curtain camera: boot manor, keep pad 1 connected via input
# script (no physical controller), walk stick-left for 1s after settle, shot.
#
#   powershell -ExecutionPolicy Bypass -File tools\debug\yn-bisect\curtain-repro.ps1
#   powershell -ExecutionPolicy Bypass -File tools\debug\yn-bisect\curtain-repro.ps1 -SettleMs 14000
#   powershell -ExecutionPolicy Bypass -File tools\debug\yn-bisect\curtain-repro.ps1 -NoShot
#
# After landing, leave the process up; live event/size bisect can continue.
# Do not move the camera. Event IDs are only stable at this fixed view.
[CmdletBinding()]
param(
    [int]$SettleMs = 12000,
    [int]$WalkMs = 1000,
    # Camera pull-back after the left walk so ~4 curtained windows fit the view.
    [int]$BackMs = 800,
    [int]$PostWalkWaitSec = 2,
    [switch]$NoShot,
    [switch]$AllPowerups
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Set-Location $root
$manual = Join-Path $root 'tools\debug\manual-session.ps1'

& powershell -ExecutionPolicy Bypass -File $manual stop | Out-Host
# Preserve live A/B flat-paint env across the NO100F_* clear.
$preserveFlat = $env:NO100F_FLAT_POS_SIZES
Remove-Item Env:NO100F_* -ErrorAction SilentlyContinue
if ($preserveFlat) { $env:NO100F_FLAT_POS_SIZES = $preserveFlat }
Remove-Item (Join-Path $root 'Play\curtain-skip-events.txt'),
            (Join-Path $root 'Play\curtain-skip-sizes.txt'),
            (Join-Path $root 'Play\curtain-skip-sig.txt'),
            (Join-Path $root 'Play\curtain-flat-events.txt'),
            (Join-Path $root 'Play\input_script.txt') -ErrorAction SilentlyContinue

# Script clock starts on the first PADRead. Keep pad 1 "connected" the whole time
# (empty hold until STICK_*) so unplugging a real pad cannot freeze the game.
$backStart = $SettleMs + $WalkMs + 200
$script = "0:A:1,$SettleMs`:STICK_LEFT:$WalkMs"
if ($BackMs -gt 0) {
    $script += ",$backStart`:STICK_DOWN:$BackMs"
}
$env:NO100F_INPUT_SCRIPT = $script
$env:NO100F_GPU_OWNER = '1'
$env:NO100F_GPU_OWNER_FILE = (Join-Path $root 'autotest\manual\curtain-owner-repro.jsonl')
# Log only after landing (~frame 350+) so event IDs stay usable for live skips.
$env:NO100F_GPU_OWNER_MIN_FRAME = '350'
$env:NO100F_GPU_OWNER_MAX = '30000'
Remove-Item $env:NO100F_GPU_OWNER_FILE -ErrorAction SilentlyContinue
Remove-Item (Join-Path $root 'Play\curtain-stick.txt') -ErrorAction SilentlyContinue

[System.IO.File]::WriteAllText(
    (Join-Path $root 'Play\curtain-test-toast.txt'),
    "REPRO boot i001 - LEFT ${WalkMs}ms then BACK ${BackMs}ms")

$startArgs = @('-ExecutionPolicy', 'Bypass', '-File', $manual, 'start', '-Boot', 'i001')
if ($AllPowerups) { $startArgs += '-AllPowerups' }
& powershell @startArgs | Out-Host

$totalMs = $SettleMs + $WalkMs + 200 + [math]::Max(0, $BackMs)
$waitSec = [math]::Ceiling($totalMs / 1000.0) + $PostWalkWaitSec + 2
Write-Host "repro: waiting ${waitSec}s for settle+walk (script=$script)"
Start-Sleep -Seconds $waitSec

[System.IO.File]::WriteAllText(
    (Join-Path $root 'Play\curtain-test-toast.txt'),
    'REPRO LANDING - do not move camera; event IDs valid only here')

if (-not $NoShot) {
    & powershell -ExecutionPolicy Bypass -File $manual shot -Label curtains-repro-landing | Out-Host
}

Write-Host "repro: done. script was: $script"
Write-Host 'repro: leave game running; bisect with Play\curtain-skip-events.txt'
Write-Host 'repro: live nudge without keyboard: write LEFT/RIGHT/UP/DOWN to Play\curtain-stick.txt'
