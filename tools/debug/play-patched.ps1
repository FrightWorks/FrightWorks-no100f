# Play a build with debug patches in it, and take them out of the source again when the game closes.
#
#   powershell -ExecutionPolicy Bypass -File tools\debug\play-patched.ps1                   # dev menu (F8)
#   ... play-patched.ps1 -Patch devmenu,postool                                            # several patches
#   ... play-patched.ps1 -SkipTranslate        # faster: only if the last build was from this branch (see AGENTS.md)
#   ... play-patched.ps1 -Keep                 # leave the patches applied (to work on them; re-export afterwards)
#   ... play-patched.ps1 -CleanBuild           # after the game closes, rebuild Play\ without the patches
#   ... play-patched.ps1 -HdTextures           # with the HD texture pack (texture_pack\ScoobyHD, as the release ships it)
#
# Steps: git apply each tools\debug\patches\<name>.patch, build, start Play\FrightWorks Game.exe, wait for it to
# exit, then git apply -R the patches this run applied. A patch that was already applied is left as found. A patch whose
# files were edited while the game ran no longer reverses cleanly: it is left applied and the script says so.
# -HdTextures links texture_pack\ScoobyHD into Play\texture_replacements (if not staged there) and sets [video] texture_replacements =
# true for this run only; both go back when the game closes (only that Config line: settings changed in game stay).
# The human is playing, so sound is on at 5% for the run ([audio] volume = 0.05, muted = false); those two lines go back
# the same way when the game closes. Rumble is off for the run too ([controller] rumble = false, no switch), and its line
# goes back the same way.
# Removal also runs on a build failure or Ctrl+C. If the window is closed instead, run with -Remove.
#   ... play-patched.ps1 -Remove [-Patch ...]  # just take the patches out
#   ... play-patched.ps1 -NoBuild [-HdTextures] # play the build already in Play\ (e.g. from texhunt\build-devmenu.ps1)
param(
    [string[]]$Patch = @('devmenu'),
    [switch]$SkipTranslate,
    [switch]$NoBuild,
    [switch]$Keep,
    [switch]$CleanBuild,
    [switch]$HdTextures,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$env:NO100F_DEV_TOOLS = '1'  # dev menu (F8) on for this session's game (built-in dev menu; older builds use the patch)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path  # this file is tools\debug\play-patched.ps1
. (Join-Path $PSScriptRoot 'game-run.ps1')     # exclusive game-run slot while the game is open (a person is playing)
. (Join-Path $PSScriptRoot 'config-lines.ps1')  # Set-ConfigLine
$patchDir = Join-Path $PSScriptRoot 'patches'
$exe = Join-Path $root 'Play\FrightWorks Game.exe'
# powershell -File passes "-Patch a,b" as one string
$names = @($Patch | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

function Git-Apply([string[]]$GitArgs) {
    & git -C $root apply @GitArgs 2>$null
    return $LASTEXITCODE -eq 0
}

$config = Join-Path $root 'Play\UserData\Config.toml'
$packLink = Join-Path $root 'Play\texture_replacements\ScoobyHD'

function Build([switch]$Translate) {
    $buildArgs = @('-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\build-no100f.ps1'), '-SkipTools')
    if (-not $Translate) { $buildArgs += '-SkipTranslate' }
    & powershell @buildArgs | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

$files = foreach ($n in $names) {
    $f = Join-Path $patchDir "$n.patch"
    if (-not (Test-Path $f)) { throw "no patch $f" }
    [pscustomobject]@{ Name = $n; File = $f }
}

if ($Remove) {
    foreach ($p in $files) {
        if (Git-Apply @('-R', '--check', $p.File)) { Git-Apply @('-R', $p.File) | Out-Null; "removed   $($p.Name)" }
        elseif (Git-Apply @('--check', $p.File)) { "not applied $($p.Name)" }
        else { "CHANGED   $($p.Name): its files differ from the patch; fix by hand (git diff)" }
    }
    return
}

if (Get-Process | Where-Object { $_.Path -eq $exe }) { throw "the game is already running from Play\; close it first" }
$applied = New-Object System.Collections.Generic.List[object]
& git -C $root cat-file -e 'HEAD:wiicompiled/runtime/src/no100f_devmenu.cpp' 2>$null
$builtInDevmenu = $LASTEXITCODE -eq 0
$hdOld = $null; $hdSet = $false; $linked = $false; $locked = $false
$soundOld = @{}; $soundSet = $false
$rumbleOld = $null; $rumbleSet = $false
try {
    if ($HdTextures) {
        $pack = Join-Path $root 'texture_pack\ScoobyHD'
        if (-not (Test-Path $pack)) { throw "no HD texture pack at $pack" }
        if (-not (Test-Path $packLink)) {
            New-Item -ItemType Directory -Force (Split-Path $packLink) | Out-Null
            New-Item -ItemType Junction -Path $packLink -Target $pack | Out-Null
            $linked = $true
        }
        $hdOld = Set-ConfigLine $config 'video' 'texture_replacements' 'texture_replacements = true'
        $hdSet = $true
        "hd        texture_pack\ScoobyHD on for this run"
    }
    # The human is playing: sound on at 5% for this run (AGENTS.md rule); the old lines go back when the game closes.
    $soundSet = $true
    $soundOld['volume'] = Set-ConfigLine $config 'audio' 'volume' 'volume = 0.05'
    $soundOld['muted'] = Set-ConfigLine $config 'audio' 'muted' 'muted = false'
    "sound     on at 5% for this run"
    # Rumble: always off in test runs (AGENTS.md rule), even here; the old line goes back when the game closes.
    $rumbleOld = Set-ConfigLine $config 'controller' 'rumble' 'rumble = false'
    $rumbleSet = $true
    "rumble    off for this run"
    foreach ($p in $files) {
        if ($NoBuild) { break }
        # The dev menu is part of the game since it was built in (NO100F_DEV_TOOLS above switches it on).
        if ($p.Name -eq 'devmenu' -and $builtInDevmenu) { "built in  devmenu (dev tools on for this run)"; continue }
        if (Git-Apply @('--check', $p.File)) {
            Git-Apply @($p.File) | Out-Null
            $applied.Add($p)
            "applied   $($p.Name)"
        } elseif (Git-Apply @('-R', '--check', $p.File)) {
            "already   $($p.Name) (applied before this run: left as found)"
        } else {
            throw "$($p.Name) does not apply to this branch (git apply --check $($p.File)); rebase it with git apply -3"
        }
    }

    if ($NoBuild) { "playing   $(Split-Path -Leaf $exe) as built (-NoBuild: no patches applied)" }
    else {
        Build -Translate:(-not $SkipTranslate)
        "playing   $(Split-Path -Leaf $exe) (patches come out when it closes)"
    }
    Enter-GameSlot $root "play-patched $($names -join ',')"
    $locked = $true
    $game = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
    $game.WaitForExit()
    "closed    exit code $($game.ExitCode)"
} finally {
    if ($locked) { Exit-GameSlot $root }
    if ($rumbleSet) {
        Set-ConfigLine $config 'controller' 'rumble' $rumbleOld | Out-Null
        "rumble    setting put back"
    }
    if ($linked) { [IO.Directory]::Delete($packLink, $false) }  # the junction only, not the pack
    if ($hdSet -or $linked) { "hd        texture pack setting and link put back" }
    if ($soundSet) {
        foreach ($k in @('muted', 'volume')) {
            if ($soundOld.ContainsKey($k)) { Set-ConfigLine $config 'audio' $k $soundOld[$k] | Out-Null }
        }
        "sound     volume and mute settings put back"
    }
    if ($hdSet) { Set-ConfigLine $config 'video' 'texture_replacements' $hdOld | Out-Null }
    if ($Keep) {
        if ($applied.Count) { "kept      $($applied.Name -join ', ') (-Keep). Remove later with -Remove" }
    } else {
        for ($i = $applied.Count - 1; $i -ge 0; --$i) {
            $p = $applied[$i]
            if (Git-Apply @('-R', '--check', $p.File)) {
                Git-Apply @('-R', $p.File) | Out-Null
                "removed   $($p.Name)"
            } else {
                "LEFT IN   $($p.Name): its files were edited since it was applied. Re-export it, then run -Remove:"
                "          git diff --output=tools/debug/patches/$($p.Name).patch HEAD -- <its files>  (git add -N new files first)"
            }
        }
    }
}

if ($CleanBuild -and -not $Keep -and $applied.Count) {
    "rebuilding Play\ without the patches"
    Build
}
