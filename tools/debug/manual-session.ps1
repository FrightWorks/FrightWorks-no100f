# Single-window manual playtest helper for texture / visual bugs.
# Never spawns workers, never scripts input, never auto-kills after N seconds.
#
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 start [-Sound]
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 shot [-Label glitch]
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 status
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 stop
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 stop-tests   # kill leftover automations only
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 prepare [-Boot ..]  # test config + boot mod, no game
#   powershell -ExecutionPolicy Bypass -File tools\debug\manual-session.ps1 restore     # put the user's setup back
#
# Every test runs with the same config (written below: windowed 1600x900, fps60, msaa 4, HD off unless -HdTextures,
# sound muted unless -Sound), not the user's own. Sound: [audio] volume = 0.05 and muted = true; -Sound (the human is
# testing or playing it, or an audio bug/enhancement) writes muted = false, so the game plays at 5%. Mute and volume are
# SDL output gain only: AUDIO_RAW_DUMP captures are the same either way.
# Rumble: always off ([controller] rumble = false), with no switch to turn it on.
# start/prepare first save the user's setup (Play\UserData\Config.toml and any *.controller files, where aurora keeps
# F10 controller remaps) to tools\debug\local\user-setup\, unless a saved copy is already there (a test is
# running, or one didn't finish: the copy there is still the user's). stop and restore put it back, delete the copy and
# remove the zz-manual boot mod. Scripts that run the game themselves: prepare, launch, then stop/restore in a finally.
#
# Worktrees: everything here stays inside this checkout (its Play\, its user-setup copy); games started from other
# checkouts and the user's own game are never touched. prepare/start take the machine-wide game-run lock (kit-common.ps1)
# and wait while another checkout holds it; stop/restore give it back.
#
# Default boot (when -Boot is omitted): e001 — first Hedge Maze section
# ("On Edge in the Hedge 1"). Override with -Boot <level> if needed.
#   ... start -Boot h002
# Unlock every eSPECIAL_* gadget in sd2.ini (springs, helmet, etc.):
#   ... start -AllPowerups
#   ... start -Boot g001 -AllPowerups
#
# Shots land in autotest\manual\. Session state in autotest\manual\session.json.
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('start', 'prepare', 'shot', 'status', 'stop', 'restore', 'stop-tests')]
    [string]$Command = 'status',

    [string]$Boot = 'e001',
    [string]$Label = '',
    [switch]$HdTextures,
    [switch]$AllPowerups,
    [switch]$Sound,
    [int]$Width = 1600,
    [int]$Height = 900
)

$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
Set-StrictMode -Version 3.0

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path  # this file is tools\debug\manual-session.ps1
. (Join-Path $PSScriptRoot 'kit-common.ps1')
$play = Join-Path $root 'Play'
$exe = Join-Path $play 'FrightWorks Game.exe'
$outDir = Join-Path $root 'autotest\manual'
$statePath = Join-Path $outDir 'session.json'
$disc = Join-Path $root 'Disc\DATA'
$userData = Join-Path $play 'UserData'
$userConfig = Join-Path $userData 'Config.toml'
$backupDir = Join-Path $root 'tools\debug\local\user-setup'
$backupMark = Join-Path $backupDir 'saved.txt'  # written last: no mark = no complete copy

# The user's Config.toml + controller files -> tools\debug\local\user-setup\ (kept if a copy is already there).
function Save-UserSetup {
    if (Test-Path -LiteralPath $backupMark) {
        $when = (Get-Content -LiteralPath $backupMark -TotalCount 1)
        Write-Host "setup: your settings were already saved ($when); keeping that copy"
        return
    }
    Remove-Item -Recurse -Force -LiteralPath $backupDir -ErrorAction SilentlyContinue
    [IO.Directory]::CreateDirectory($backupDir) | Out-Null
    $saved = @()
    if (Test-Path -LiteralPath $userConfig) {
        Copy-Item -LiteralPath $userConfig (Join-Path $backupDir 'Config.toml') -Force
        $saved += 'Config.toml'
    }
    foreach ($f in @(Get-ChildItem -LiteralPath $userData -Filter '*.controller' -File -ErrorAction SilentlyContinue)) {
        Copy-Item -LiteralPath $f.FullName (Join-Path $backupDir $f.Name) -Force
        $saved += $f.Name
    }
    # line 1: when; the rest: the files saved (a missing Config.toml is deleted again on restore)
    @((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) + $saved | Set-Content -LiteralPath $backupMark -Encoding utf8
    Write-Host "setup: saved your settings ($(if ($saved) { $saved -join ', ' } else { 'no Config.toml yet' }))"
}

# Puts the saved copy back (files the test made that weren't there before are deleted) and removes zz-manual.
function Restore-UserSetup {
    Remove-Item -Recurse -Force -LiteralPath (Join-Path $userData 'mods\zz-manual') -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $backupMark)) {
        Write-Host 'setup: nothing saved to restore'
        return
    }
    $saved = @(Get-Content -LiteralPath $backupMark | Select-Object -Skip 1 | Where-Object { $_ })
    if ($saved -contains 'Config.toml') {
        Copy-Item -LiteralPath (Join-Path $backupDir 'Config.toml') $userConfig -Force
    } else {
        Remove-Item -LiteralPath $userConfig -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $userData -Filter '*.controller' -File -ErrorAction SilentlyContinue |
        Where-Object { $saved -notcontains $_.Name } | Remove-Item -Force
    foreach ($name in @($saved | Where-Object { $_ -ne 'Config.toml' })) {
        Copy-Item -LiteralPath (Join-Path $backupDir $name) (Join-Path $userData $name) -Force
    }
    Remove-Item -Recurse -Force -LiteralPath $backupDir
    Write-Host "setup: restored your settings ($(if ($saved) { $saved -join ', ' } else { 'none existed' }))"
}

function Stop-AutomationLeftovers {
    $patterns = @(
        '_verify_playground', '_verify_fix', '_repro_multi', '_probe_i001',
        'level-sweep', 'verify-playground', 'verify-fix', 'NO100F_INPUT_SCRIPT'
    )
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | ForEach-Object {
        $cmd = $_.CommandLine
        $path = $_.ExecutablePath
        if (-not $cmd -and -not $path) { return }
        $hit = $false
        foreach ($p in $patterns) {
            if (($cmd -and $cmd -match [regex]::Escape($p)) -or ($path -and $path -match [regex]::Escape($p))) {
                $hit = $true; break
            }
        }
        # Any game copy running from this checkout's autotest\ (never from Play\, never another checkout's).
        if ($path -and $path.StartsWith((Join-Path $root 'autotest') + '\', [StringComparison]::OrdinalIgnoreCase)) { $hit = $true }
        if ($hit) {
            Write-Host "automation: kill $($_.ProcessId) $($_.Name)"
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-PlayGameProcesses {
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -match '^FrightWorks Game\.exe$|Scooby' -and $_.ExecutablePath -and (
            $_.ExecutablePath -eq $exe -or
            ((Split-Path $_.ExecutablePath -Parent) -eq $play)
        )
    }
}

function Read-Session {
    if (-not (Test-Path -LiteralPath $statePath)) { return $null }
    return (Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json)
}

function Write-Session([hashtable]$data) {
    [IO.Directory]::CreateDirectory($outDir) | Out-Null
    ($data | ConvertTo-Json) | Set-Content -LiteralPath $statePath -Encoding utf8
}

function Assert-PlayReady {
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "Missing $exe - build with tools\build-no100f.ps1 first."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $disc 'sys\main.dol'))) {
        throw "Missing Disc\DATA - extract the ISO first."
    }
}

function Write-PlayConfig([string]$bootLevel, [bool]$unlockAllPowerups) {
    [IO.Directory]::CreateDirectory((Join-Path $play 'UserData')) | Out-Null
    # Never leave a sweep mod that overrides BOOT behind.
    Remove-Item -Recurse -Force (Join-Path $play 'UserData\mods\zz-sweep') -ErrorAction SilentlyContinue

    $dvd = ($disc -replace '\\', '/')
    $hd = if ($HdTextures) { 'true' } else { 'false' }
    $muted = if ($Sound) { 'false' } else { 'true' }
    @"
[paths]
dvd_root = '$dvd'

[video]
display_mode = "windowed"
window_width = $Width
window_height = $Height
resolution_multiplier = 0
aspect_mode = 1
skip_intro = true
fps60 = true
msaa = 4
texture_replacements = $hd

[audio]
volume = 0.05
muted = $muted

[controller]
rumble = false
"@ | Set-Content -LiteralPath (Join-Path $play 'UserData\Config.toml') -Encoding utf8

    Write-Host "sound: $(if ($Sound) { 'on at 5% (-Sound)' } else { 'muted (-Sound: on at 5%)' })"

    $needIni = [bool]$bootLevel -or $unlockAllPowerups
    if (-not $needIni) {
        Remove-Item -Recurse -Force (Join-Path $play 'UserData\mods\zz-manual') -ErrorAction SilentlyContinue
        Write-Host 'boot: game default / menu (no BOOT override, stock powerups)'
        return
    }

    $modFiles = Join-Path $play 'UserData\mods\zz-manual\files'
    [IO.Directory]::CreateDirectory($modFiles) | Out-Null
    $iniPath = Join-Path $disc 'files\sd2.ini'
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $ini = [IO.File]::ReadAllText($iniPath, $latin1)
    if ($bootLevel) {
        $ini = [regex]::Replace($ini, '(?m)^BOOT=.*$', "BOOT=$bootLevel")
        $ini = [regex]::Replace($ini, '(?m)^ShowMenuOnBoot=1', 'ShowMenuOnBoot=0')
        Write-Host "boot override: BOOT=$bootLevel (single window, no input script)"
    } else {
        Write-Host 'boot: game default / menu (no BOOT override)'
    }
    if ($unlockAllPowerups) {
        # Disc sd2.ini ships every eSPECIAL_* at 0; set all of them to 1 via mod overlay.
        $ini = [regex]::Replace($ini, '(?m)^(eSPECIAL_\S+\s*=\s*)\d+', '${1}1')
        $count = ([regex]::Matches($ini, '(?m)^eSPECIAL_\S+\s*=\s*1')).Count
        Write-Host "powerups: unlocked $count eSPECIAL_* flags (springs, helmet, etc.)"
    }
    [IO.File]::WriteAllText((Join-Path $modFiles 'sd2.ini'), $ini, $latin1)
}

switch ($Command) {
    'stop-tests' {
        Stop-AutomationLeftovers
        Write-Host 'stop-tests: done'
    }
    'stop' {
        Stop-AutomationLeftovers
        foreach ($p in Get-PlayGameProcesses) {
            Write-Host "stop: kill Play instance $($p.ProcessId)"
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $statePath -ErrorAction SilentlyContinue
        Write-Host 'stop: session cleared'
        Restore-UserSetup
        Exit-GameLock $root
    }
    'restore' {
        if (@(Get-PlayGameProcesses).Count) { Write-Host 'restore: the game is still running; it may write its config when it closes' }
        Restore-UserSetup
        Exit-GameLock $root
    }
    'prepare' {
        Assert-PlayReady
        if (@(Get-PlayGameProcesses).Count) { throw 'prepare: the game is running from Play\; close it (or run stop) first' }
        Enter-GameLock $root "manual-session prepare -Boot $Boot"
        Save-UserSetup
        Write-PlayConfig -bootLevel $Boot -unlockAllPowerups:([bool]$AllPowerups)
        Write-Host 'prepare: test config + boot mod written; launch the game yourself, then stop (or restore) when done'
    }
    'status' {
        $session = Read-Session
        $procs = @(Get-PlayGameProcesses)
        if ($procs.Count -eq 0) {
            Write-Host 'status: no Play game running'
            if ($session) { Write-Host "status: stale session.json pid=$($session.pid)" }
            exit 1
        }
        foreach ($p in $procs) {
            Write-Host "status: alive pid=$($p.ProcessId) path=$($p.ExecutablePath)"
        }
        if ($session) {
            Write-Host "status: session boot=$($session.boot) allPowerups=$($session.allPowerups) sound=$(if ($session.PSObject.Properties['sound']) { $session.sound } else { '?' }) started=$($session.started)"
        }
        if ($procs.Count -gt 1) {
            Write-Host 'status: WARNING more than one Play instance - use stop then start'
            exit 2
        }
    }
    'start' {
        Assert-PlayReady
        Stop-AutomationLeftovers
        $existing = @(Get-PlayGameProcesses)
        if ($existing.Count -gt 0) {
            Write-Host "start: already running pid=$($existing[0].ProcessId) - not launching another"
            Write-Host 'start: use "shot" to capture, or "stop" then "start" to relaunch'
            exit 0
        }
        # Other game copies from this checkout (autotest\ copies; shouldn't be any after stop-tests). Games from other
        # checkouts and the user's own are left alone: the game-run lock below keeps test runs apart.
        $strays = @(Get-CheckoutGames $root | Where-Object { $_.ExecutablePath -ne $exe })
        if ($strays) {
            foreach ($s in $strays) {
                Write-Host "start: killing stray $($s.ProcessId) $($s.ExecutablePath)"
                Stop-Process -Id $s.ProcessId -Force -ErrorAction SilentlyContinue
            }
            Start-Sleep -Seconds 1
        }
        Enter-GameLock $root "manual-session start -Boot $Boot"
        Save-UserSetup
        Write-PlayConfig -bootLevel $Boot -unlockAllPowerups:([bool]$AllPowerups)
        # Inherit caller env into the game process (Start-Process default). Log selected diags if set.
        foreach ($name in @(
                'NO100F_TEX_NOCACHE', 'NO100F_NO_TEXMEMO', 'NO100F_FLAT_TEST', 'NO100F_NO_MERGE',
                'NO100F_INPUT_SCRIPT', 'NO100F_SCRIPT_REMOTES',
                'NO100F_IDX_CHECK', 'NO100F_CURTAIN_TRACE', 'NO100F_TAIL_SKIP', 'NO100F_VTX_IDX', 'NO100F_SKIP_LINES',
                'NO100F_SKIP_DEEP_IDX', 'NO100F_SKIP_CURTAIN_PANEL', 'NO100F_SKIP_CURTAIN_HEM',
                'NO100F_DROP_DEGEN_IDX', 'NO100F_GPU_OWNER', 'NO100F_GPU_OWNER_FILE',
                'NO100F_GPU_OWNER_FRAME', 'NO100F_GPU_OWNER_MAX', 'NO100F_SKIP_GPU_EVENTS',
                'NO100F_XF_REJECT_LOG', 'NO100F_NTG0_REPAIR', 'NO100F_SKIP_POS_SIZES'
            )) {
            $v = [Environment]::GetEnvironmentVariable($name, 'Process')
            if ($null -ne $v -and $v -ne '') {
                Write-Host "diag: $name=$v (inherited by game)"
            }
        }
        $p = Start-Process -FilePath $exe -WorkingDirectory $play -PassThru
        Write-Session @{
            pid         = $p.Id
            boot        = $Boot
            allPowerups = [bool]$AllPowerups
            sound       = [bool]$Sound
            started     = (Get-Date).ToString('o')
            exe         = $exe
        }
        Write-Host "start: launched pid=$($p.Id)"
        Write-Host "start: exe=$exe"
        Write-Host 'start: walk wherever you want; run "shot" to capture, "stop" when done'
        Write-Host 'start: Curtains F8 or Xbox LB: once at window (trace ON + shot); flutter hems; again to stop (trace OFF + shot)'
    }
    'shot' {
        # Do not kill anything here - only capture the Play window.
        $procs = @(Get-PlayGameProcesses)
        if ($procs.Count -eq 0) { throw 'shot: no Play game running - use start first' }
        if ($procs.Count -gt 1) { throw 'shot: multiple Play instances - run stop, then start' }
        $gamePid = [int]$procs[0].ProcessId
        [IO.Directory]::CreateDirectory($outDir) | Out-Null
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $suffix = if ($Label) { "-$Label" } else { '' }
        $out = Join-Path $outDir "shot-$stamp$suffix.png"
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\shot-pid.ps1') `
            -ProcessId $gamePid -Out $out
        if (-not (Test-Path -LiteralPath $out)) { throw "shot: capture failed for pid=$gamePid" }
        $bytes = (Get-Item -LiteralPath $out).Length
        Write-Host "shot: $out ($bytes bytes)"
    }
}
