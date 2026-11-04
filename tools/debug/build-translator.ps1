# build-translator.ps1: rebuild the translator (wiicompiled\translator\src\Translator.Cli, Release, no incremental).
# tools\build-no100f.ps1 builds it only when the dll is missing, so after switching to or from a branch that changes
# wiicompiled\translator (e.g. upstream PR #23) run this, then build WITHOUT -SkipTranslate (a full translate).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dotnet = Get-Item (& (Join-Path $root 'tools\get-dotnet.ps1') | Select-Object -Last 1)   # portable .NET 10 SDK
$env:DOTNET_ROLL_FORWARD = 'Major'; $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
& $dotnet.FullName build (Join-Path $root 'wiicompiled\translator\src\Translator.Cli') -c Release --no-incremental *>&1 |
    Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { throw 'translator build failed' }
"translator built ($(git -C $root rev-parse --short HEAD) on $(git -C $root branch --show-current))"
