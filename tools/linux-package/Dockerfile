# Build environment for the Linux release package (tools/package-linux.sh, started by tools/package-linux-docker.ps1).
# Ubuntu 22.04 on purpose: the package then needs no newer glibc than 22.04's 2.35, so it runs on Ubuntu 22.04 and newer,
# Debian 12, Fedora, Arch and SteamOS. Ubuntu's own clang is 14 and can't build the game, so clang 18 comes from LLVM's
# apt repository (its signing key is checked against the fingerprint below). The other packages are the ones the README
# lists for a native Linux build, plus what packaging and the headless checks need (xvfb, squashfs-tools for vpk).
# The base image is pinned by digest (the multi-platform index of ubuntu:22.04).
FROM ubuntu:22.04@sha256:076e7bf82319619f28dd0aa9c02ab34b055ad37100395b4f49a2ffba04fb5e11

ENV DEBIAN_FRONTEND=noninteractive \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
    DOTNET_gcServer=0

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg git rsync xz-utils unzip file binutils \
        ninja-build python3 python3-pil imagemagick librsvg2-bin libarchive-tools pkg-config \
        libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev libxtst-dev libasound2-dev \
        squashfs-tools desktop-file-utils xvfb xauth libssl3 \
    && rm -rf /var/lib/apt/lists/*

# clang 18 + the libstdc++ 12 headers it builds against (README: "Ubuntu 22.04's clang is 14, which fails").
RUN set -eux; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.key; \
    gpg --show-keys --with-colons /tmp/llvm.key | awk -F: '$1=="fpr"{print $10}' | grep -qx 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421; \
    gpg --dearmor < /tmp/llvm.key > /usr/share/keyrings/llvm.gpg; rm /tmp/llvm.key; \
    echo 'deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' > /etc/apt/sources.list.d/llvm.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends clang-18 libstdc++-12-dev; \
    rm -rf /var/lib/apt/lists/*; \
    ln -s /usr/bin/clang-18 /usr/local/bin/clang; ln -s /usr/bin/clang++-18 /usr/local/bin/clang++

# patchelf: the package script points the game at the libraries shipped beside it. zstd: vpk needs it for delta packages.
RUN apt-get update && apt-get install -y --no-install-recommends patchelf zstd && rm -rf /var/lib/apt/lists/*

# The source is a clone made by the script from a read-only mount of the repository's .git, owned by someone else.
RUN git config --system safe.directory '*'

# Windows checkouts have CRLF in their files: this copy is fixed here.
COPY entry.sh /usr/local/bin/frightworks-entry
RUN sed -i 's/\r$//' /usr/local/bin/frightworks-entry && chmod 755 /usr/local/bin/frightworks-entry

WORKDIR /work
ENTRYPOINT ["frightworks-entry"]
