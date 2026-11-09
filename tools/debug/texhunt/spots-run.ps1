# spots-run.ps1 -Boot h001 -Ids T001,T002 -Mode orig|hd|clean: boot a scene, go to each saved dev-menu spot, screenshot.
# Needs the dev menu build in Play\. Runs with the test config (manual-session prepare, HD set by -Mode); afterwards
# the texture link is removed and manual-session restore puts your settings back and removes zz-manual.
# Spots saved with the free camera on get its pose, so when every wanted spot has one the run jumps straight to them.
# Otherwise (the game camera only turns at certain points) it goes through ALL of the scene's spots in saved order
# (devmenu_spots.txt) up to the last wanted one, and shoots only the wanted ones (-Pass seconds at the others).
# The free camera goes off before every spot (a spot with a pose turns it back on).
# -Branch <branch>: build Play\ from that branch + dev menu first (build-devmenu.ps1), e.g. main = without PR #22.
# -Pack <folder>: the HD pack for -Mode hd (default texture_pack\ScoobyHD): a full path, or a folder of autotest\texhunt.
param([string]$Boot = 'h001', [string]$Ids = '', [string]$Mode = 'hd', [int]$Settle = 3, [int]$Pass = 1, [int]$Wait = 0, [string]$Tag = '',
      [string]$Branch = '', [string]$Pack = '')
$label = if ($Tag) { "$Mode-$Tag" } else { $Mode }  # file name suffix: <spot>-<Mode>[-<Tag>].png
$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path   # repo root
. (Join-Path $PSScriptRoot '..\game-run.ps1')  # Stop-CheckoutGames: only this checkout's game
Set-Location $root
$play = Join-Path $root 'Play'
$exe = Join-Path $play 'FrightWorks Game.exe'
$ms = Join-Path $root 'tools\debug\manual-session.ps1'
$cfg = Join-Path $play 'UserData\Config.toml'
$cmdFile = Join-Path $play 'devmenu.txt'
$logFile = Join-Path $play 'devmenu.log'
$link = Join-Path $play 'texture_replacements\ScoobyHD'
$outDir = Join-Path $root 'autotest\texhunt\spots'
New-Item -ItemType Directory -Force $outDir | Out-Null
if ($Branch) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'tools\debug\texhunt\build-devmenu.ps1') -Branch $Branch
    if ($LASTEXITCODE -ne 0) { throw "dev menu build of $Branch failed" }
}
$start = [DateTime]::Now
$linked = $false  # only a link this script made is removed (builds stage the HD pack there as a real folder)
$idList = @($Ids -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
# this scene's spot names in saved order (line: SCENE x y z yaw [cam=...] name)
$camSpots = @()
$route = @(Get-Content (Join-Path $play 'UserData\devmenu_spots.txt') | ForEach-Object {
        $f = $_ -split ' ', 6
        if ($f.Count -eq 6 -and $f[0] -eq $Boot.ToUpper()) {
            $name = ($f[5] -replace '^cam=\S+ ', '').Trim()
            if ($f[5] -match '^cam=') { $camSpots += $name }
            $name
        }
    })
$missing = @($idList | Where-Object { $route -notcontains $_ })
if ($missing.Count) { throw "no $Boot spot named $($missing -join ', ') in devmenu_spots.txt" }
if (-not $idList.Count) { throw 'no -Ids' }
if (@($idList | Where-Object { $camSpots -notcontains $_ }).Count -eq 0) {
    $route = @($route | Where-Object { $idList -contains $_ })  # all have a camera pose: jump straight there
} else {
    $last = ($idList | ForEach-Object { [Array]::IndexOf($route, $_) } | Measure-Object -Maximum).Maximum
    $route = @($route[0..$last])
}

function Newest-Console { Get-ChildItem (Join-Path $play 'UserData\Logs') -Directory | Sort-Object CreationTime -Descending | Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName 'console.log' } }
function Run-Cmd($line, $timeout = 90) {
    $before = if (Test-Path $logFile) { @(Get-Content $logFile).Count } else { 0 }
    Set-Content -Path $cmdFile -Value $line
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $timeout) {
        if ((Test-Path $logFile) -and @(Get-Content $logFile).Count -gt $before) { return (Get-Content $logFile)[-1] }
        Start-Sleep -Milliseconds 300
    }
    Remove-Item $cmdFile -ErrorAction SilentlyContinue
    return "$line -> TIMEOUT"
}

try {
    & powershell -ExecutionPolicy Bypass -File $ms prepare -Boot $Boot | Out-Null  # saves your settings, writes boot mod + Config
    if ($LASTEXITCODE -ne 0) { throw 'manual-session prepare failed (game running?)' }
    # manual-session's boot mod only sets BOOT: add the hub intro skip (as hunt.ps1 does) or the shots catch the cutscene
    if ($Boot -eq 'h001') {
        & python (Join-Path $root 'tools\debug\texhunt\skipintro.py') (Join-Path $play 'UserData\mods\zz-manual') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'skipintro.py failed' }
    }
    # the test config manual-session wrote; only the HD setting changes
    $hdOn = $Mode -ne 'orig'
    $text = [IO.File]::ReadAllText($cfg) -replace '(?m)^texture_replacements\s*=[^\r\n]*\r?\n', ''
    $text = [regex]::Replace($text, '(?m)^\[video\][ \t]*\r?\n', "[video]`r`ntexture_replacements = $(if ($hdOn) { 'true' } else { 'false' })`r`n")
    [IO.File]::WriteAllText($cfg, $text)
    if ($hdOn -and $Mode -eq 'hd' -and -not $Pack -and (Test-Path $link)) {
        # the build staged texture_pack\ScoobyHD here already: play that
    } elseif ($hdOn) {
        if (Test-Path $link) { throw "$link exists (move the staged pack away to test another one)" }
        $target = if ($Mode -eq 'clean') { Join-Path $root 'autotest\texhunt\cleanpack' }
                  elseif ($Pack -and [IO.Path]::IsPathRooted($Pack)) { $Pack }
                  elseif ($Pack) { Join-Path $root "autotest\texhunt\$Pack" }
                  else { Join-Path $root 'texture_pack\ScoobyHD' }
        if (-not (Test-Path $target -PathType Container)) { throw "no pack folder $target" }
        New-Item -ItemType Junction -Path $link -Target $target | Out-Null
        $linked = $true
    }
    Remove-Item $logFile, $cmdFile -ErrorAction SilentlyContinue
    $game = Start-Process -FilePath $exe -WorkingDirectory $play -PassThru
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt 120) {
        $c = Newest-Console
        if ($c -and (Test-Path $c) -and (Get-Item (Split-Path $c)).CreationTime -gt $start -and (Select-String -Path $c -Pattern "$Boot.HIP" -SimpleMatch -Quiet)) { break }
        Start-Sleep -Milliseconds 500
    }
    "where: " + (Run-Cmd 'where' 120)
    Start-Sleep $Wait
    foreach ($id in $route) {
        $r = Run-Cmd @('freecam off', "spot $id")
        if ($idList -notcontains $id) { "pass $id"; Start-Sleep $Pass; continue }
        Start-Sleep $Settle
        & powershell -ExecutionPolicy Bypass -File $ms shot -Label "$id-$label" | Out-Null
        $shot = Get-ChildItem (Join-Path $root 'autotest\manual') -Filter "shot-*-$id-$label.png" | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($shot) { Move-Item $shot.FullName (Join-Path $outDir "$id-$label.png") -Force }
        "$id : $r"
    }
    & powershell -ExecutionPolicy Bypass -File $ms stop | Out-Null
} finally {
    Stop-CheckoutGames $root
    Start-Sleep 1
    if ($linked) { [IO.Directory]::Delete($link, $false) }  # the junction only
    & powershell -ExecutionPolicy Bypass -File $ms restore | Out-Null  # your settings back, zz-manual removed
    Remove-Item $cmdFile -ErrorAction SilentlyContinue
    "restored"
}
