# Builds the Linux release package (tools/package-linux.sh) in an Ubuntu 22.04 container, from Windows or any host with
# Docker (Docker Desktop must already be running; this script doesn't start it).
#   powershell -ExecutionPolicy Bypass -File tools\package-linux-docker.ps1 [-Version <v>] [-Feed <dir>] [-NoDownload]
#                                                                           [-Jobs <n>] [-Clean] [-Shell]
# The package is built from the COMMITTED state of the current branch (the container clones the repository's .git and
# checks out HEAD), never from the working copy: commit first. A clean checkout at the tag v<version> gives the plain
# version as the build ID, like the Windows release.
# Where things live:
#  - the Docker volume -Volume (default frightworks-linux-build): the clone, the Linux toolchain (CMake, .NET SDK, vpk),
#    the game and launcher build folders. It is kept between runs, so the second run reuses the toolchain and the
#    compiled code. -Clean removes the volume first (a build from nothing). Remove it by hand when done:
#    docker volume rm frightworks-linux-build
#  - read only mounts from the main checkout: Disc\ (the extracted disc), texture_pack\ and game\ (main.dol).
#  - -Feed (default dist\installer-linux): the finished AppImage, packages and releases.linux.json. Old files in it are
#    kept (the delta needs the previous package), so clear test builds out of it before a release.
# -Run '<bash command>' runs that in the clone (at the commit, with the toolchain and build folders) instead of packaging, for
# checks such as running the runtime tests; the run needs no -Feed. -Jobs caps the parallel compile jobs (default: RAM / 1.5 GB, at most 16 and the core count). -Shell opens a shell in the
# container instead of packaging (for looking around and for checks: the volume is mounted at /work, the clone is
# /work/src).
[CmdletBinding()] param(
    [string]$Version, [string]$Feed, [switch]$NoDownload, [int]$Jobs = 0, [switch]$Clean, [switch]$Shell, [string]$Run,
    [string]$Volume = 'frightworks-linux-build'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$image = 'frightworks-linux-package:2'  # bump with a change to tools\linux-package\Dockerfile or entry.sh

& docker info --format '{{.ServerVersion}}' *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker is not running. Start Docker Desktop and run this again.' }

if (-not $Shell -and -not $Run -and (git -C $root status --porcelain --untracked-files=no)) {
    throw 'There are uncommitted changes to tracked files. The package is built from the last commit: commit them first.'
}
$sha = (git -C $root rev-parse HEAD).Trim()
$branch = (git -C $root rev-parse --abbrev-ref HEAD).Trim()
if ($branch -eq 'HEAD') { $branch = '' }
$gitDir = (git -C $root rev-parse --path-format=absolute --git-common-dir).Trim()
$mainRoot = Split-Path -Parent $gitDir

# Disc, texture_pack and game: a plain clone has them as ordinary folders, a git worktree as links to the main checkout's
# (mount what the link points at; the main checkout is found from git, not from a fixed path). A missing texture_pack or
# game is fine: the container's build downloads the HD pack and takes main.dol from the disc. The disc is required.
function Resolve-Mount([string]$name) {
    foreach ($base in $root, $mainRoot) {
        $item = Get-Item -LiteralPath (Join-Path $base $name) -Force -ErrorAction SilentlyContinue
        if (-not $item) { continue }
        if ($item.LinkType -and $item.Target) { $item = Get-Item -LiteralPath ([string]($item.Target | Select-Object -First 1)) }
        return $item.FullName
    }
    return $null
}
$disc = Resolve-Mount 'Disc'; $pack = Resolve-Mount 'texture_pack'; $game = Resolve-Mount 'game'
if (-not $disc -or -not (Test-Path -LiteralPath (Join-Path $disc 'DATA\sys\boot.bin'))) {
    throw 'No extracted disc in Disc\DATA. Extract your disc first (README: Building), like the Windows build needs.'
}

if (-not $Feed) { $Feed = Join-Path $root 'dist\installer-linux' }
New-Item -ItemType Directory -Force $Feed | Out-Null
$Feed = (Resolve-Path $Feed).Path

& docker build -q -t $image "$PSScriptRoot\linux-package" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'docker build failed' }
if ($Clean) { & docker volume rm -f $Volume | Out-Null }

$dockerArgs = @('run', '--rm', '-i', '-v', "${Volume}:/work", '-v', "${gitDir}:/host-git:ro", '-v', "${disc}:/ro/Disc:ro")
if ($pack) { $dockerArgs += @('-v', "${pack}:/ro/texture_pack:ro") }
if ($game) { $dockerArgs += @('-v', "${game}:/ro/game:ro") }
$dockerArgs += @('-v', "${Feed}:/out", '-e', "HOST_SHA=$sha", '-e', "HOST_BRANCH=$branch")
if ($Jobs -gt 0) { $dockerArgs += @('-e', "FRIGHTWORKS_JOBS=$Jobs") }
if ($Shell) { $dockerArgs += '-t' }
$dockerArgs += $image
$args2 = @()
if ($Shell) { $args2 += '--shell' }
elseif ($Run) { $args2 += @('--run', $Run) }
else {
    if ($Version) { $args2 += @('--version', $Version) }
    if ($NoDownload) { $args2 += '--no-download' }
}
& docker @dockerArgs @args2
if ($LASTEXITCODE -ne 0) { throw "the container build failed (exit $LASTEXITCODE)" }
if (-not $Shell -and -not $Run) {
    Write-Host "DONE: $Feed"
    Get-ChildItem -LiteralPath $Feed -File | Sort-Object LastWriteTime | ForEach-Object { '  {0}  {1:N0} MB' -f $_.Name, ($_.Length / 1MB) }
}
