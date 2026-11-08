#!/bin/sh
# Diagnostic: one parallel game run per draw range a-b (draws a..b of each frame skipped), each shot in Mystic Manor at 40 s.
# Boots straight into the level through a temporary sd2.ini mod (ShowMenuOnBoot=0), removed afterwards.
# Usage: sh tools/bisect-draws.sh 0-499 500-999 ...   -> autotest/bisect-<range>.png (+ .err.log with [drawcount])
cd "$(dirname "$0")/.."
mod=Play/UserData/mods/zz-boot/files
mkdir -p "$mod" && sed 's/^ShowMenuOnBoot=1/ShowMenuOnBoot=0/' Disc/DATA/files/sd2.ini > "$mod/sd2.ini"
for r in "$@"; do
  NO100F_NO_MERGE=1 NO100F_SKIP_DRAWS="$r" NO100F_INPUT_SCRIPT="12000:A:200,20000:A:200,28000:A:200" \
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/capture-window.ps1 -Seconds 40 -Out "autotest/bisect-$r.png" > /dev/null 2>&1 &
done
wait
rm -rf Play/UserData/mods/zz-boot
