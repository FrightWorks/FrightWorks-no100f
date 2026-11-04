# Returns the path of a dotnet.exe with the .NET 10 SDK: the portable one in toolchain\dotnet, installed there on first
# use with Microsoft's dotnet-install.ps1 (nothing is installed system-wide). A toolchain\dotnet from before the move to
# .NET 10 gets the new SDK beside its old one.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'toolchain\dotnet'
$dotnet = Join-Path $dir 'dotnet.exe'
function Test-Sdk {
    if (-not (Test-Path -LiteralPath $dotnet)) { return $false }
    try { return @(& $dotnet --list-sdks | Where-Object { $_ -match '^10\.' }).Count -gt 0 } catch { return $false }
}
if (-not (Test-Sdk)) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    # The install replaces dotnet.exe: build servers left running from this folder would lock it.
    if (Test-Path -LiteralPath $dotnet) { try { & $dotnet build-server shutdown | Out-Null } catch { } }
    $installer = Join-Path $dir 'dotnet-install.ps1'
    Write-Host "Downloading the .NET 10 SDK into $dir ..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing 'https://dot.net/v1/dotnet-install.ps1' -OutFile $installer
    & $installer -Channel 10.0 -InstallDir $dir -NoPath | Out-Host
    if (-not (Test-Sdk)) { throw ".NET 10 SDK install failed ($dir)" }
}
$dotnet
