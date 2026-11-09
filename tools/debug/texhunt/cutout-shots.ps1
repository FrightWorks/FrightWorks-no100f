# cutout-shots.ps1 -Label <name> [-Boot h001] [-Spot Hub_Example_2] [-Hd | -Pack <full path>] [-Modes 0,1,2,3] [-Interp 120|180]:
# needs a Play\ build with aurora's cut-out modes + the dev menu (feat/hd-cutout-twopass). Opens the game at the spot
# (look.ps1), sets each cut-out mode with the dev menu's "cutout N" command, shoots it
# (autotest\manual\shot-*-cutout-<label>-m<N>.png), then closes the game (look.ps1 restores the setup) and writes a
# 2x2 grid autotest\manual\cmp-cutout-<label>.jpg.
param([Parameter(Mandatory)][string]$Label, [string]$Boot = 'h001', [string]$Spot = 'Hub_Example_2', [switch]$Hd,
      [string]$Pack = '', [string]$Modes = '0,1,2,3', [int]$Interp = 0)
if ($Spot -eq 'none') { $Spot = '' }  # boot start position
$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path   # repo root
. (Join-Path $PSScriptRoot '..\game-run.ps1')  # Get-/Stop-CheckoutGames: only this checkout's game
Set-Location $root
$ms = Join-Path $root 'tools\debug\manual-session.ps1'
$cmd = Join-Path $root 'Play\devmenu.txt'
$log = Join-Path $root 'Play\devmenu.log'
$out = Join-Path $env:TEMP "cutout-look-$PID.txt"
$modeList = $Modes -split ',' | ForEach-Object { [int]$_ }

# Start-Process joins the arguments without quoting: quote the paths (they contain spaces)
$lookArgs = @('-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $root 'tools\debug\texhunt\look.ps1')`"", '-Boot', $Boot)
if ($Spot) { $lookArgs += @('-Spot', $Spot) }
if ($Pack) { $lookArgs += @('-Pack', "`"$Pack`"") } elseif ($Hd) { $lookArgs += '-Hd' }
if ($Interp) { $lookArgs += @('-Interp', $Interp) }
$look = Start-Process powershell -ArgumentList $lookArgs -PassThru -NoNewWindow -RedirectStandardOutput $out
try {
    $t = Get-Date
    while (-not ((Test-Path $out) -and (Select-String $out -Pattern 'game open' -Quiet))) {
        if ($look.HasExited -or ((Get-Date) - $t).TotalSeconds -gt 180) { throw "look.ps1 did not open the game: $(Get-Content $out -Raw)" }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep 3
    $shots = @()
    foreach ($m in $modeList) {
        Remove-Item $log -ErrorAction SilentlyContinue
        Set-Content $cmd "cutout $m"
        $t = Get-Date
        while (-not (Test-Path $log) -and ((Get-Date) - $t).TotalSeconds -lt 15) { Start-Sleep -Milliseconds 200 }
        if (-not ((Test-Path $log) -and (Get-Content $log -Raw) -match "cutout mode $m")) { throw "mode $m not confirmed: $(Get-Content $log -Raw -ErrorAction SilentlyContinue)" }
        Start-Sleep 1
        $line = & powershell -ExecutionPolicy Bypass -File $ms shot -Label "cutout-$Label-m$m" | Select-Object -Last 1
        if ($line -notmatch 'shot: (.+\.png)') { throw "shot failed: $line" }
        $shots += $Matches[1]
        "mode $m  $($Matches[1])"
    }
} finally {
    Get-CheckoutGames $root | ForEach-Object { (Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue).CloseMainWindow() | Out-Null }
    if (-not $look.WaitForExit(30000)) { Stop-CheckoutGames $root; $look.WaitForExit(15000) | Out-Null }
    Get-Content $out | Select-String 'restored|closed' | ForEach-Object Line
    Remove-Item $out -ErrorAction SilentlyContinue
}
$grid = Join-Path $root "autotest\manual\cmp-cutout-$Label.jpg"
New-Item -ItemType Directory -Force (Split-Path $grid) | Out-Null
& python -c @"
import sys
from PIL import Image, ImageDraw
names = {0: '0 game', 1: '1 cut (#22)', 2: '2 two-pass HD', 3: '3 two-pass all'}
fs, modes = sys.argv[2:], [int(m) for m in sys.argv[1].split(',')]
ims = [Image.open(f).convert('RGB') for f in fs]
w, h = ims[0].size; tw, th = w // 2, h // 2
g = Image.new('RGB', (tw * 2, th * ((len(ims) + 1) // 2)))
for i, (im, m) in enumerate(zip(ims, modes)):
    t = im.resize((tw, th)); d = ImageDraw.Draw(t); d.rectangle((0, 0, 200, 22), fill=(0, 0, 0)); d.text((6, 5), names[m], fill=(255, 255, 0))
    g.paste(t, ((i % 2) * tw, (i // 2) * th))
g.save(r'$grid', quality=90)
"@ ($modeList -join ',') @shots
"grid      $grid"
