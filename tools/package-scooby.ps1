# Assembles dist\FrightWorks: one exe on top (the launcher, named "FrightWorks.exe"), README.txt, and
# data\ with the game's runtime files. No disc data in the package (but the HD pack fix reads the repo's Disc\DATA).
#   powershell -ExecutionPolicy Bypass -File tools\package-scooby.ps1 [-Build] [-Zip]
#   powershell -ExecutionPolicy Bypass -File tools\package-scooby.ps1 -Installer [-Version <v>] [-Feed <dir>] [-NoDownload]
# -Build first rebuilds the game (build-no100f.ps1 -SkipTools) and the launcher.
# -Zip also writes dist\FrightWorks.zip from a clean copy (never the player's disc, saves or settings).
# Updating dist in place only replaces program files: data\Disc and data\UserData (saves, Config.toml, mods) are kept,
# so a disc set up there by the launcher survives a repackage. An old Play\ layout is moved into data\ first.
# -Installer packs the same program files, without the portable marker, with Velopack (vpk, fetched into dist\.vpk) into
# -Feed (default dist\installer): FrightWorks-win-Setup.exe, the full package, a delta from the previous release, and
# releases.win.json, which the installed launcher reads from GitHub Releases. It first downloads the latest GitHub
# release's package into the feed so the delta can be made (-NoDownload: use what the feed holds, for a local test feed).
# -Version defaults to <Version> in the launcher's .csproj. Uploading is a separate step (docs/testing.md).
[CmdletBinding()] param([switch]$Build, [switch]$Zip, [switch]$Installer, [string]$Version, [string]$Feed, [switch]$NoDownload)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$launcher = "$root\launcher\publish\FrightWorks.exe"
$VpkVersion = '1.2.161'  # keep in step with the Velopack PackageReference in the launcher's .csproj
$RepoUrl = 'https://github.com/FrightWorks/FrightWorks-no100f'
if ($Build) { & "$PSScriptRoot\build-no100f.ps1" -SkipTools; & "$PSScriptRoot\build-launcher.ps1" }
elseif (-not (Test-Path -LiteralPath $launcher)) { & "$PSScriptRoot\build-launcher.ps1" }

# Program files only: what a new download replaces. -Installed: no portable marker (the game then keeps the player's
# files in %LOCALAPPDATA%\FrightWorks-Data and Saved Games\FrightWorks), no UserData, no zip README.
function Stage([string]$to, [switch]$Installed) {
    New-Item -ItemType Directory -Force "$to\data" | Out-Null
    if (-not $Installed) { New-Item -ItemType Directory -Force "$to\data\UserData\mods" | Out-Null }
    Copy-Item -LiteralPath $launcher -Destination "$to\FrightWorks.exe" -Force
    if (-not $Installed) { Copy-Item -LiteralPath "$PSScriptRoot\package-README.txt" -Destination "$to\README.txt" -Force }
    $files = 'FrightWorks Game.exe', '*.dll', 'dsp_coef.bin', 'initial_pipeline_cache.db'
    if (-not $Installed) { $files += 'portable.txt' }
    foreach ($f in $files) {
        if (Test-Path "$root\Play\$f") { Copy-Item "$root\Play\$f" "$to\data" -Force }
    }
    # Built-in HD texture pack (texture_pack\ScoobyHD, kept out of git) and the button prompt icons: the same files
    # every Play\ build gets, beside the game (program files, not the player's).
    & "$PSScriptRoot\stage-assets.ps1" -GameDir "$to\data" -Required
}

$dist = "$root\dist\FrightWorks"
New-Item -ItemType Directory -Force $dist | Out-Null
# Old layout (Play\ + Disc\ + "...Launcher.exe"): keep the player's UserData and disc, drop the rest.
if (Test-Path "$dist\Play") {
    New-Item -ItemType Directory -Force "$dist\data" | Out-Null
    if ((Test-Path "$dist\Play\UserData") -and -not (Test-Path "$dist\data\UserData")) { Move-Item "$dist\Play\UserData" "$dist\data\UserData" }
    if ((Test-Path "$dist\Disc\DATA") -and -not (Test-Path "$dist\data\Disc\DATA")) {
        New-Item -ItemType Directory -Force "$dist\data\Disc" | Out-Null; Move-Item "$dist\Disc\DATA" "$dist\data\Disc\DATA"
    }
    # The launcher wrote dvd_root relative to Play\UserData; data\Disc sits one level closer.
    $cfg = "$dist\data\UserData\Config.toml"
    if (Test-Path $cfg) { (Get-Content $cfg) -replace "^dvd_root = '../../Disc/DATA'", "dvd_root = '../Disc/DATA'" | Set-Content $cfg }
    Remove-Item -Recurse -Force "$dist\Play", "$dist\Disc" -ErrorAction SilentlyContinue
    Remove-Item -Force "$dist\Scooby-Doo Refrightened Launcher.exe" -ErrorAction SilentlyContinue
}
Stage $dist
$size = [math]::Round((Get-ChildItem -LiteralPath $dist -Recurse -File | ? FullName -notmatch '\\data\\(Disc|UserData)\\' | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "DONE: $dist ($size MB of program files; data\Disc and data\UserData left as they were)"
if ($Zip) {
    $clean = Join-Path $env:TEMP "FrightWorks-zip\FrightWorks"
    Remove-Item -Recurse -Force (Split-Path $clean) -ErrorAction SilentlyContinue
    Stage $clean
    # Entries with forward slashes (Compress-Archive in Windows PowerShell 5.1 writes backslashes, which some
    # unzip tools reject).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zipPath = "$root\dist\FrightWorks.zip"
    Remove-Item -LiteralPath $zipPath -ErrorAction SilentlyContinue
    $archive = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
    try {
        $base = Split-Path $clean
        foreach ($f in Get-ChildItem -LiteralPath $clean -Recurse -File) {
            $name = $f.FullName.Substring($base.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $name, 'Optimal') | Out-Null
        }
        $archive.CreateEntry('FrightWorks/data/UserData/mods/') | Out-Null  # keep the empty mods folder
    } finally { $archive.Dispose() }
    Remove-Item -Recurse -Force (Split-Path $clean)
    Write-Host "DONE: $root\dist\FrightWorks.zip"
}
if ($Installer) {
    if (-not $Version) {
        $Version = ([xml](Get-Content -Raw "$root\launcher\FrightWorks.Launcher.csproj")).Project.PropertyGroup.Version | Where-Object { $_ } | Select-Object -First 1
    }
    if (-not $Feed) { $Feed = "$root\dist\installer" }
    New-Item -ItemType Directory -Force $Feed | Out-Null
    # The title names the shortcuts and the uninstall entry: no ':' (on NTFS "A: B.lnk" becomes a stream of a file "A").
    # vpk (Velopack's packer), a .NET tool, pinned and kept in dist\.vpk; it runs on the .NET 10 SDK in toolchain\dotnet.
    $dotnet = & "$PSScriptRoot\get-dotnet.ps1" | Select-Object -Last 1
    $env:DOTNET_ROOT = Split-Path $dotnet; $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'; $env:DOTNET_NOLOGO = '1'
    $vpkDir = "$root\dist\.vpk\$VpkVersion"
    $vpk = "$vpkDir\vpk.exe"
    if (-not (Test-Path -LiteralPath $vpk)) {
        & $dotnet tool install vpk --version $VpkVersion --tool-path $vpkDir
        if ($LASTEXITCODE -ne 0) { throw "installing vpk $VpkVersion failed" }
    }
    $clean = Join-Path $env:TEMP "FrightWorks-installer\FrightWorks"
    Remove-Item -Recurse -Force (Split-Path $clean) -ErrorAction SilentlyContinue
    Stage $clean -Installed
    if (-not $NoDownload) {
        # The latest release's full package, for the delta (none yet before the first installer release: fine).
        & $vpk download github --repoUrl $RepoUrl --outputDir $Feed
        if ($LASTEXITCODE -ne 0) { throw "vpk download github failed" }
    }
    & $vpk pack --packId FrightWorks --packVersion $Version --packDir $clean --mainExe FrightWorks.exe --runtime win-x64 `
        --packTitle FrightWorks --packAuthors Loneranger419 --icon "$root\launcher\frightworks.ico" `
        --noPortable --outputDir $Feed
    if ($LASTEXITCODE -ne 0) { throw "vpk pack failed" }
    Remove-Item -Recurse -Force (Split-Path $clean)
    Write-Host "DONE: $Feed (FrightWorks $Version)"
    Get-ChildItem -LiteralPath $Feed -File | Sort-Object LastWriteTime | ForEach-Object { "  {0}  {1:N0} MB" -f $_.Name, ($_.Length / 1MB) }
}
