#!/usr/bin/env bash
# Runs inside the container (tools/package-linux-docker.ps1 starts it). The source is a clone, kept in the /work volume with
# the toolchain and build folders, of the repository mounted read-only at /host-git, checked out at exactly the commit the
# caller names: the package is built from committed state, never from a working copy.
#   entry.sh [--shell | --run '<command>'] [package-linux.sh arguments]
# Environment: HOST_SHA (commit), HOST_BRANCH (its branch name, or empty when detached), FRIGHTWORKS_JOBS (optional).
# Mounts: /host-git (the repository's .git, read only), /ro/Disc, /ro/texture_pack, /ro/game (read only, from the main
# checkout), /out (the feed folder).
set -euo pipefail

src=/work/src
mkdir -p /work
[[ -n "${HOST_SHA:-}" ]] || { echo "HOST_SHA is not set." >&2; exit 2; }

if [[ ! -d "$src/.git" ]]; then
    git clone --quiet --no-checkout --no-hardlinks /host-git "$src"
fi
cd "$src"
git tag -l | xargs -r git tag -d >/dev/null  # tags come from the host only (a test tag there must not outlive it here)
git fetch --quiet --force --no-tags /host-git '+refs/heads/*:refs/remotes/host/*' '+refs/tags/*:refs/tags/*'
if [[ -n "${HOST_BRANCH:-}" ]]; then
    git checkout --quiet --force -B "$HOST_BRANCH" "$HOST_SHA"
else
    git checkout --quiet --force --detach "$HOST_SHA"
fi
git submodule status >/dev/null 2>&1 || true
echo "Building commit $(git rev-parse --short=12 HEAD) ($(git log -1 --format=%s)), branch ${HOST_BRANCH:-detached}"
echo "Tags on it: $(git tag --points-at HEAD | tr '\n' ' ')"

# The user's disc and the game's main.dol come from the main checkout, mounted read only. The HD pack is copied: the build
# adds the Creeper frames (tools/hd-pack-additions) to texture_pack/ScoobyHD, which must not touch the original.
# The folders the build writes (toolchain, Play, wiicompiled/generated and native-build-linux, dist) are this volume's own.
# Without a texture_pack mount the build downloads the pack into the volume; without a game mount main.dol is taken from the
# disc.
[[ -e /ro/Disc/DATA/sys/boot.bin ]] || { echo "/ro/Disc/DATA is not mounted." >&2; exit 2; }
ln -sfn /ro/Disc "$src/Disc"
[[ -L "$src/game" ]] && rm "$src/game"
if [[ -e /ro/game ]]; then ln -sfn /ro/game "$src/game"; else mkdir -p "$src/game"; fi
[[ -L "$src/texture_pack" ]] && rm "$src/texture_pack"
mkdir -p "$src/texture_pack"
if [[ -e /ro/texture_pack ]]; then rsync -a /ro/texture_pack/ "$src/texture_pack/"; fi

if [[ -z "${FRIGHTWORKS_JOBS:-}" ]]; then
    # About 1.5 GB of RAM per job for the compiler, at most one per core, at most 16.
    mem_gb=$(awk '/MemTotal/ {printf "%d", $2 / 1048576}' /proc/meminfo)
    jobs=$(( mem_gb * 2 / 3 )); (( jobs < 1 )) && jobs=1
    cores=$(nproc); (( jobs > cores )) && jobs=$cores
    (( jobs > 16 )) && jobs=16
    export FRIGHTWORKS_JOBS=$jobs
fi
echo "Jobs: $FRIGHTWORKS_JOBS"

if [[ "${1:-}" == "--shell" ]]; then
    shift
    exec bash "$@"
fi
if [[ "${1:-}" == "--run" ]]; then  # --run '<command>': a command in the clone, for checks (package-linux-docker.ps1 -Run)
    exec bash -c "${2:?--run needs a command}"
fi
exec tools/package-linux.sh --build --feed /out "$@"
