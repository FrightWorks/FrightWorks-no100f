# hunt.ps1 [-SkipTranslate] [-Boot h001]: texture-hunt session. Dev menu build with the HD pack (play-patched.ps1
# -HdTextures), Scooby in the hub with every powerup and no intro cutscene, game audio dumped for the stuck-music check.
#
# Mod zz-hunt (removed when the game closes): sd2.ini with BOOT=<Boot>, ShowMenuOnBoot=0, every eSPECIAL_*=1, and for
# h001 the intro-skip HIP (skipintro.py). skip_intro in Config.toml already skips the logos and Intro.bik.
# After the game closes: loopcheck.py --scan 10 on the dump (autotest\audio-loop\hunt.raw); a window near 1.0 = stuck.
# -Pack <folder>: play that pack instead of texture_pack\ScoobyHD (linked in its place; play-patched leaves a link it
# didn't make alone, this script removes it): a full path (e.g. the HD-4X repo's work\pack) or a folder of autotest\texhunt.
# -Branch <branch>: build that branch + dev menu (build-devmenu.ps1; the source comes straight back to the current
# branch) and play the build. Needs a clean tree apart from untracked files.
# -TexLog: the dev menu's texture log on from the start (Play\devmenu_texlog.csv, appended; delete it for a fresh log).
param([string]$Boot = 'h001', [string]$Pack = '', [switch]$SkipTranslate, [string]$Branch = '', [switch]$TexLog)
$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path   # repo root
. (Join-Path $PSScriptRoot '..\game-run.ps1')  # Get-CheckoutGames: only this checkout's game
Set-Location $root
$mods = Join-Path $root 'Play\UserData\mods'
$mod = Join-Path $mods 'zz-hunt'
$dump = Join-Path $root 'autotest\audio-loop\hunt.raw'
New-Item -ItemType Directory -Force (Split-Path $dump) | Out-Null
$packLink = Join-Path $root 'Play\texture_replacements\ScoobyHD'

if (@(Get-CheckoutGames $root).Count) { throw 'the game is running from this checkout; close it first' }
if ($Branch) {
    # build-devmenu.ps1 builds the branch and comes back to this one (the kit may not exist on that branch)
    "hunt      building $Branch + dev menu"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-devmenu.ps1') -Branch $Branch
    if ($LASTEXITCODE -ne 0) { throw "dev menu build of $Branch failed" }
}
try {
    # zz-manual (manual-session leftover) would also bring an sd2.ini
    Remove-Item -Recurse -Force (Join-Path $mods 'zz-manual'), $mod -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $mod 'files') | Out-Null
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $ini = [IO.File]::ReadAllText((Join-Path $root 'Disc\DATA\files\sd2.ini'), $latin1)
    $ini = [regex]::Replace($ini, '(?m)^BOOT=.*$', "BOOT=$Boot")
    $ini = [regex]::Replace($ini, '(?m)^ShowMenuOnBoot=1', 'ShowMenuOnBoot=0')
    $ini = [regex]::Replace($ini, '(?m)^(eSPECIAL_\S+\s*=\s*)\d+', '${1}1')
    [IO.File]::WriteAllText((Join-Path $mod 'files\sd2.ini'), $ini, $latin1)
    if ($Boot -eq 'h001') {
        & python (Join-Path $root 'tools\debug\texhunt\skipintro.py') $mod
        if ($LASTEXITCODE -ne 0) { throw 'skipintro.py failed' }
    }
    if ($Pack) {
        if (Test-Path $packLink) { throw "$packLink exists" }
        New-Item -ItemType Directory -Force (Split-Path $packLink) | Out-Null
        $packPath = if ([IO.Path]::IsPathRooted($Pack)) { $Pack } else { Join-Path $root "autotest\texhunt\$Pack" }
        if (-not (Test-Path $packPath -PathType Container)) { throw "no pack folder $packPath" }
        New-Item -ItemType Junction -Path $packLink -Target $packPath | Out-Null
        $linked = $true
        "hunt      texture pack = $packPath"
    }
    "hunt      BOOT=$Boot, all powerups$(if ($Boot -eq 'h001') { ', no intro cutscene' })"

    Remove-Item $dump -ErrorAction SilentlyContinue
    $env:AUDIO_RAW_DUMP = $dump
    if ($TexLog) { $env:NO100F_TEXLOG = '1' }
    $playArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\debug\play-patched.ps1'), '-HdTextures')
    if ($Branch) { $playArgs += '-NoBuild' } elseif ($SkipTranslate) { $playArgs += '-SkipTranslate' }
    & powershell @playArgs
} finally {
    Remove-Item Env:AUDIO_RAW_DUMP, Env:NO100F_TEXLOG -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $mod -ErrorAction SilentlyContinue
    if ($linked) { [IO.Directory]::Delete($packLink, $false) }  # the junction only
    "hunt      zz-hunt removed"
}
if (Test-Path $dump) {
    $scan = & python (Join-Path $root 'tools\debug\audio\loopcheck.py') $dump --scan 10
    $stuck = @($scan | Where-Object { $_ -match 'STUCK' })
    "music     $($scan[0]); $(if ($stuck.Count) { "$($stuck.Count) STUCK windows:" } else { 'no stuck loop' })"
    $stuck | Select-Object -First 20
}
