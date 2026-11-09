# look.ps1 -Boot e005 -Spot Greenhouse_Example_1 [-Hd | -Pack <full path>] [-Extra <folder>] [-AllPowerups] [-Fps30] [-Interp 120|180] [-SkipButton a|none] [-KeepIntro] [-Sound]: open the game (dev menu build already in Play\) at a saved spot
# and leave it to the human. HD textures off unless -Hd; sound muted unless -Sound (then on at 5%). stderr (RT_LOGF)
# goes to autotest\texhunt\look-stderr.log.
# -Extra <folder>: also link that folder of replacements (e.g. tint-pack.py output, frames you made) as
# texture_replacements\00-extra; it is indexed before ScoobyHD, so its textures win (turns replacements on).
# Runs with the test config (manual-session prepare; the switches above only change HD/fps60/interpolation in it).
# When the game closes: the links removed, manual-session restore puts your settings back and removes zz-manual.
param([string]$Boot = 'e005', [string]$Spot = '', [switch]$Hd, [switch]$AllPowerups, [switch]$Fps30, [string]$Pack = '', [string]$Extra = '', [int]$Interp = 0, [string]$SkipButton = 'a', [switch]$KeepIntro, [switch]$Sound)
if ($Pack) { $Hd = $true }  # -Pack <folder>: that pack instead of texture_pack\ScoobyHD (full path)
if ($Extra) { $Extra = (Resolve-Path $Extra).Path }
$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path   # repo root
. (Join-Path $PSScriptRoot '..\game-run.ps1')  # Get-CheckoutGames: only this checkout's game
Set-Location $root
$play = Join-Path $root 'Play'
$exe = Join-Path $play 'FrightWorks Game.exe'
$ms = Join-Path $root 'tools\debug\manual-session.ps1'
$cfg = Join-Path $play 'UserData\Config.toml'
$cmdFile = Join-Path $play 'devmenu.txt'
$logFile = Join-Path $play 'devmenu.log'
$link = Join-Path $play 'texture_replacements\ScoobyHD'
$extraLink = Join-Path $play 'UserData\texture_replacements\00-extra'
$made = @()   # links this script made (only those are removed)
$errLog = Join-Path $root 'autotest\texhunt\look-stderr.log'
New-Item -ItemType Directory -Force (Split-Path $errLog) | Out-Null
$start = [DateTime]::Now

if (@(Get-CheckoutGames $root).Count) { throw 'the game is running from this checkout; close it first' }
try {
    # boot mod + test config from manual-session prepare (saves your settings first), then the HD/fps switches
    $msArgs = @('-ExecutionPolicy', 'Bypass', '-File', $ms, 'prepare', '-Boot', $Boot)
    if ($AllPowerups) { $msArgs += '-AllPowerups' }
    if ($Sound) { $msArgs += '-Sound' }
    & powershell @msArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'manual-session prepare failed' }
    if ($Boot -eq 'h001' -and -not $KeepIntro) {
        & python (Join-Path $root 'tools\debug\texhunt\skipintro.py') (Join-Path $play 'UserData\mods\zz-manual') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'skipintro.py failed' }
    }
    $text = [IO.File]::ReadAllText($cfg) -replace '(?m)^texture_replacements\s*=[^\r\n]*\r?\n', ''
    $text = [regex]::Replace($text, '(?m)^\[video\][ \t]*\r?\n', "[video]`r`ntexture_replacements = $(if ($Hd -or $Extra) { 'true' } else { 'false' })`r`n")
    if ($Fps30) { $text = $text -replace '(?m)^fps60\s*=\s*true', 'fps60 = false' }
    if ($Interp) {
        $text = $text -replace '(?m)^frame_interpolation_fps\s*=[^\r\n]*\r?\n', ''
        $text = [regex]::Replace($text, '(?m)^\[video\][ \t]*\r?\n', "[video]`r`nframe_interpolation_fps = $Interp`r`n")
    }
    [IO.File]::WriteAllText($cfg, $text)
    if ($Hd) {
        if (Test-Path $link) { throw "$link exists" }
        New-Item -ItemType Directory -Force (Split-Path $link) | Out-Null
        New-Item -ItemType Junction -Path $link -Target $(if ($Pack) { $Pack } else { Join-Path $root 'texture_pack\ScoobyHD' }) | Out-Null
        $made += $link
    }
    if ($Extra) {
        if (Test-Path $extraLink) { throw "$extraLink exists" }
        New-Item -ItemType Directory -Force (Split-Path $extraLink) | Out-Null
        New-Item -ItemType Junction -Path $extraLink -Target $Extra | Out-Null
        $made += $extraLink
    }
    Remove-Item $logFile, $cmdFile -ErrorAction SilentlyContinue
    $game = Start-Process -FilePath $exe -WorkingDirectory $play -PassThru -RedirectStandardError $errLog
    "look      $Boot, HD $(if ($Hd) { 'on' } else { 'off' })$(if ($Extra) { ", extra $Extra" }), pid $($game.Id)"
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt 120) {
        $c = Get-ChildItem (Join-Path $play 'UserData\Logs') -Directory | Sort-Object CreationTime -Descending | Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName 'console.log' }
        if ($c -and (Test-Path $c) -and (Get-Item (Split-Path $c)).CreationTime -gt $start -and (Select-String -Path $c -Pattern "$Boot.HIP" -SimpleMatch -Quiet)) { break }
        Start-Sleep -Milliseconds 500
    }
    # wait until the game is in play (the dev menu answers devmenu.txt only in play; an in-world camera pan can come
    # first), then skip a recorded intro cutscene (first visit only): A (-SkipButton) every 2 s while the dev menu's
    # "cutscene" says yes (the game takes the skip only after a few seconds). Dialogue boxes are not skipped.
    Start-Sleep 2
    Remove-Item $logFile -ErrorAction SilentlyContinue
    Set-Content -Path $cmdFile -Value 'where'
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt 120 -and -not ((Test-Path $logFile) -and (Get-Content $logFile) -match '^where')) { Start-Sleep -Milliseconds 300 }
    "look      in play after $([int]([DateTime]::Now - $t).TotalSeconds) s: " + $(if (Test-Path $logFile) { @(Get-Content $logFile | Where-Object { $_ -match '^where' })[-1] } else { 'where not answered' })
    $press = Join-Path $play 'devmenu_press.txt'
    $tc = [DateTime]::Now
    $presses = 0
    while ($SkipButton -ne 'none' -and ([DateTime]::Now - $tc).TotalSeconds -lt 120) {
        Remove-Item $logFile -ErrorAction SilentlyContinue
        Set-Content -Path $cmdFile -Value 'cutscene'
        $tw = [DateTime]::Now
        while (([DateTime]::Now - $tw).TotalSeconds -lt 10 -and -not ((Test-Path $logFile) -and (Get-Content $logFile) -match '^cutscene')) { Start-Sleep -Milliseconds 200 }
        if (-not ((Test-Path $logFile) -and (Get-Content $logFile) -match 'cutscene yes')) { break }
        Set-Content -Path $press -Value "$SkipButton 10 cutscene"
        ++$presses
        Start-Sleep 2
    }
    if ($presses) { "look      cutscene skipped with $presses press(es) after $([int]([DateTime]::Now - $tc).TotalSeconds) s" }
    # then wait for player control (an in-world camera pan, e.g. the i001 key, can't be skipped)
    $tc = [DateTime]::Now
    while (([DateTime]::Now - $tc).TotalSeconds -lt 60) {
        Remove-Item $logFile -ErrorAction SilentlyContinue
        Set-Content -Path $cmdFile -Value 'cutscene'
        $tw = [DateTime]::Now
        while (([DateTime]::Now - $tw).TotalSeconds -lt 10 -and -not ((Test-Path $logFile) -and (Get-Content $logFile) -match '^cutscene')) { Start-Sleep -Milliseconds 200 }
        if ((Test-Path $logFile) -and (Get-Content $logFile) -match 'control yes') { break }
        Start-Sleep 1
    }
    "look      player control after $([int]([DateTime]::Now - $tc).TotalSeconds) s more"
    if ($Spot) {
        Start-Sleep 2
        Set-Content -Path $cmdFile -Value @('freecam off', "spot $Spot")
        $t = [DateTime]::Now
        while (([DateTime]::Now - $t).TotalSeconds -lt 60 -and -not ((Test-Path $logFile) -and (Get-Content $logFile) -match 'spot|Teleport')) { Start-Sleep -Milliseconds 300 }
        "look      " + $(if (Test-Path $logFile) { (Get-Content $logFile)[-1] } else { 'spot command not answered' })
    }
    "look      game open; cleanup runs when it closes"
    $game.WaitForExit()
    "closed    exit code $($game.ExitCode)"
} finally {
    foreach ($l in $made) { if (Test-Path $l) { [IO.Directory]::Delete($l, $false) } }
    & powershell -ExecutionPolicy Bypass -File $ms restore | Out-Null  # your settings back, zz-manual removed
    Remove-Item $cmdFile, (Join-Path $play 'devmenu_press.txt') -ErrorAction SilentlyContinue
    "restored"
}
