#!/usr/bin/env bash
# Linux counterpart of tools/build-launcher.ps1: the launcher as one self-contained file, launcher/publish/FrightWorks
# (no .NET install needed to run it). Uses the portable .NET 10 SDK in toolchain/dotnet, which
# tools/build-no100f-linux.sh installs; build the game first so the footer shows the build ID.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dotnet="$root/toolchain/dotnet/dotnet"
"$dotnet" --list-sdks 2>/dev/null | grep -q '^10\.' ||
    { echo "No .NET 10 SDK in toolchain/dotnet: run tools/build-no100f-linux.sh first." >&2; exit 1; }
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

rm -rf "$root/launcher/publish"
"$dotnet" publish "$root/launcher/FrightWorks.Launcher.csproj" -c Release -r linux-x64 --self-contained true \
    -p:PublishSingleFile=true -o "$root/launcher/publish"
echo "DONE: $root/launcher/publish/FrightWorks"
