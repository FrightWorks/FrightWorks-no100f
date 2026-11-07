# Third-Party Notices

WiiCompiled itself is licensed under the GNU General Public License v3.0
(see [`LICENSE`](LICENSE)). It incorporates, links against, or redistributes the third-party
components listed below. Each remains under its own license and copyright.

Nothing listed here is Nintendo intellectual property. This project ships no game code, assets,
or data of any kind - see the [README](README.md).

---

## Bundled in this repository

### aurora - MIT

Copyright (c) 2022 Luke Street.
Source: <https://github.com/encounter/aurora> - vendored in `aurora-main/`, license text at
`aurora-main/LICENSE`.

Aurora itself vendors:

- **magic_enum** 0.7.2 - MIT, Copyright (c) 2019-2021 Daniil Goncharov.
  `aurora-main/include/magic_enum.hpp`.
  Source: <https://github.com/Neargye/magic_enum>
- **libogc-derived SRAM structures** - zlib-style license, Copyright (c) Michael Wiedenbauer
  (shagkur) and Dave Murphy (WinterMute). `aurora-main/lib/card/SRAM.hpp`.
  Source: <https://github.com/devkitPro/libogc>

### Dolphin Emulator data files - GPL-2.0-or-later

Copyright (c) 2003+ Dolphin Emulator Project.
Source: <https://github.com/dolphin-emu/dolphin> - license at
<https://github.com/dolphin-emu/dolphin/blob/master/COPYING>

Two data sets from Dolphin's `Data/Sys` tree are redistributed here under GPL-2.0-or-later:

| File(s) | Upstream path | Notes |
| --- | --- | --- |
| `runtime/assets/dsp/dsp_coef.bin` | `Data/Sys/GC/dsp_coef.bin` | Free DSP polyphase-resampling coefficient ROM written by the Dolphin team. 4096 bytes, SHA-256 `D7741279C2E8EC5C5FB318F8FBDD6DE6BF583520D288E836A5383233A4238179`. The runtime verifies this hash at build time. |
| `runtime/assets/wii/shared2/wc24/**` | `Data/Sys/Wii/shared2/wc24` | Dolphin's unmodified default WiiConnect24 bootstrap tree, used to seed a newly created per-user NAND on first run. It is byte-identical to the upstream directory. `nwc24dl.bin` contains Dolphin's default task list, which references Nintendo endpoints shut down in 2014; `nwc24msg.cfg` contains Dolphin's placeholder account fields, not a real user's account data. |

Neither contains Nintendo executable code or game assets; both are redistributed under
GPL-2.0-or-later from Dolphin's `Data/Sys` tree.

SHA-256 hashes for the WiiConnect24 bootstrap tree:

| File | SHA-256 |
| --- | --- |
| `runtime/assets/wii/shared2/wc24/misc.bin` | `13DD5B6B2682DEFD3B23AFD8E2983D00EDC25BD4DC28A8389380DEE0EC45A4A5` |
| `runtime/assets/wii/shared2/wc24/nwc24dl.bin` | `057B6F840C19B41CE080318BC7E717E2B910965CE72AB781A7E319017636C38E` |
| `runtime/assets/wii/shared2/wc24/nwc24fl.bin` | `ED94AF416C47ED3BC2C944EBCD1D734B8935D9697FEB0F7039D8FEA3EC514C18` |
| `runtime/assets/wii/shared2/wc24/nwc24fls.bin` | `C3A4A5649D6ED2322A0DE98D2258B96A6A1D3C0179854FD21E9835D529736822` |
| `runtime/assets/wii/shared2/wc24/nwc24msg.cbk` | `7AFEBF33EEB0035397CC74E15E892E700CD2903641D26562F5D46CFBB6171109` |
| `runtime/assets/wii/shared2/wc24/nwc24msg.cfg` | `7AFEBF33EEB0035397CC74E15E892E700CD2903641D26562F5D46CFBB6171109` |
| `runtime/assets/wii/shared2/wc24/mbox/Readme.txt` | `E5A888912968050C6C1D46D1C364C324684E1D15AAA62CFE36CF7FCE2C687B21` |
| `runtime/assets/wii/shared2/wc24/mbox/wc24recv.ctl` | `EFA39268E7071941E4FE429D49C86D73BEE952DF95D91D7909C478DD1BC9050A` |
| `runtime/assets/wii/shared2/wc24/mbox/wc24recv.mbx` | `DD2AD8C9FB38884523459963BFAEC5D5AEAA5FD20EFCDC209764D461E690E435` |
| `runtime/assets/wii/shared2/wc24/mbox/wc24send.ctl` | `430C3795F1A0AEB198BF626A4A2FF6D123321D453807DD7B904DC3B74DB35D13` |
| `runtime/assets/wii/shared2/wc24/mbox/wc24send.mbx` | `C248DC031CE09F7BE1E55956B6F173E79D6A47D913C22A16593C4687325692B7` |

Dolphin was also used extensively as a behavioural reference during development of this project's
hardware and IOS high-level implementations.

### Dolphin Emulator Riivolution code - GPL-2.0-or-later

Copyright (c) 2021 Dolphin Emulator Project.
The runtime's Riivolution patch handling is a port of Dolphin's
`Source/Core/DiscIO/RiivolutionParser.{h,cpp}` and the external-path resolution rules of
`Source/Core/DiscIO/RiivolutionPatcher.cpp`, adapted in
`runtime/include/hle/riivolution_contract.h` and `runtime/src/hle/storage/riivolution.cpp`
(both marked `SPDX-License-Identifier: GPL-2.0-or-later`).
Source: <https://github.com/dolphin-emu/dolphin>

### pugixml - MIT

Copyright (c) 2006-2025 Arseny Kapoulkine.
The Riivolution XML reader uses pugixml 1.15, vendored in
`runtime/third_party/pugixml` from commit `ee86beb30e4973f5feffe3ce63bfa4fbadf72f38`.
Source and license: <https://github.com/zeux/pugixml>

### Crypto++ 8.9.0 - Boost Software License 1.0 / public domain

Copyright (c) 1995-2019 Wei Dai and contributors.
The runtime uses Crypto++ for SHA-1 and sect233r1 ECDSA key derivation and signing. Its portable
sources are vendored in `runtime/third_party/cryptopp`; assembly implementations are disabled.
Source: <https://github.com/weidai11/cryptopp/tree/CRYPTOPP_8_9_0>. Full license text:
`runtime/third_party/cryptopp/License.txt`.

### toml11 4.4.0 - MIT

Copyright (c) 2017 Toru Niina.
The runtime configuration reader and scalar string writer use the single-header distribution,
vendored in `runtime/third_party/toml11`.
Source: <https://github.com/ToruNiina/toml11/tree/v4.4.0>. Full license text:
`runtime/third_party/toml11/LICENSE`.

### YamlDotNet - MIT

Copyright (c) Antoine Aubry and contributors.
Referenced by `translator/src/Translator.Core`. Source: <https://github.com/aaubry/YamlDotNet>

### libco - ISC (valgrind.h: BSD-style)

Copyright byuu and the higan team.
Non-Windows builds use libco's symmetric stackful coroutines in place of Win32 Fibers for guest
OSThread scheduling (`runtime/src/fiber_manager.cpp`). Vendored in full (all non-Windows
CPU-architecture backends - amd64, x86, arm, aarch64, ppc, ppc64v2, plus the portable sjlj
fallback - though this project's x86_64-only target only ever compiles amd64.c) in
`runtime/third_party/libco` from commit `e18e09d634d612a01781168ad4d76be10a7e3bad`.
Source: <https://github.com/higan-emu/libco>. Full license text:
`runtime/third_party/libco/LICENSE`.

---

## Fetched at build time and redistributed in release builds

These are pinned in `aurora-main/extern/CMakeLists.txt`, `aurora-main/CMakeLists.txt`,
`aurora-main/cmake/AuroraDawnProvider.cmake`, and (for Mbed TLS) `runtime/CMakeLists.txt`. They are
not stored in this repository; the build downloads them - each fetch is pinned to an exact version
with a checked SHA-256 - and links or redistributes the resulting binaries. Their license texts are
included in the installer's `licenses/` folder. The Windows installer bundles the pinned source
trees themselves (fetched by `Launcher/Prepare-Dependencies.ps1`) so end-user builds run offline.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| Mbed TLS | 3.6.7 | Apache-2.0 / GPL-2.0-or-later | <https://github.com/Mbed-TLS/mbedtls> |
| Dawn (WebGPU) | `v20260603.191052` prebuilt | BSD-3-Clause | <https://dawn.googlesource.com/dawn> |
| Tint (part of Dawn) | with Dawn | BSD-3-Clause | <https://dawn.googlesource.com/dawn> |
| DirectXShaderCompiler (`dxcompiler.dll`) | with Dawn | NCSA / University of Illinois Open Source | <https://github.com/microsoft/DirectXShaderCompiler> |
| SDL | 3.4.4 | zlib | <https://github.com/libsdl-org/SDL> |
| libusb (linked into SDL on Windows) | 1.0.30 | LGPL-2.1-or-later | <https://github.com/libusb/libusb> |
| Abseil | LTS 20240722.0 | Apache-2.0 | <https://github.com/abseil/abseil-cpp> |
| Dear ImGui | 1.91.9b-docking | MIT | <https://github.com/ocornut/imgui> |
| {fmt} | 11.1.4 | MIT | <https://github.com/fmtlib/fmt> |
| xxHash | 0.8.4 | BSD-2-Clause | <https://github.com/Cyan4973/xxHash> |
| zlib | 1.3.2 | zlib | <https://github.com/madler/zlib> |
| libpng | 1.6.58 | PNG Reference Library License v2 | <https://github.com/pnggroup/libpng> |
| FreeType | 2.14.3 | **FreeType License (FTL)** - see below | <https://freetype.org/> |
| Zstandard | 1.5.7 | **BSD-3-Clause** - see below | <https://github.com/facebook/zstd> |
| SQLite | 3.51.3 amalgamation | Public domain | <https://sqlite.org/> |
| Tracy Profiler | pinned commit | BSD-3-Clause | <https://github.com/wolfpld/tracy> |
| C++/WinRT | - | MIT (Microsoft) | <https://github.com/microsoft/cppwinrt> |
| nodtool (disc image extraction) | v2.0.0-alpha.10 | MIT OR Apache-2.0 | <https://github.com/encounter/nod> |

### Dual-licensed components - elections made by this project

- **FreeType** is offered under the FreeType License (FTL) or GPL-2.0. **This project elects the
  FreeType License.** The FTL requires the following credit, which is given here and reproduced in
  distributed builds:

  > Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org).
  > All rights reserved.

- **Zstandard** is offered under BSD-3-Clause or GPL-2.0. **This project elects BSD-3-Clause.**
  Copyright (c) Meta Platforms, Inc. and affiliates.

## Launcher (`launcher/`) - NuGet packages built into `FrightWorks.exe`

The launcher is an Avalonia application. Its packages are restored from nuget.org at build time (versions as
restored for the current build: `launcher/FrightWorks.Launcher.csproj` and `obj/project.assets.json`) and
end up inside the single-file `FrightWorks.exe`, with their native libraries unpacked next to it when it
starts. Where a row says Windows or Linux, it is only used there.

| Component | Version | License | Upstream |
| --- | --- | --- | --- |
| Avalonia (`Avalonia`, `.Desktop`, `.Themes.Fluent`, `.Skia`, `.HarfBuzz`, `.Win32`, `.X11`, `.FreeDesktop` and the other `Avalonia.*` packages they pull in) | 12.1.5 | MIT, Copyright 2013-2026 The AvaloniaUI Project | <https://github.com/AvaloniaUI/Avalonia> |
| SkiaSharp (`SkiaSharp`, `SkiaSharp.NativeAssets.Win32` / `.Linux`) | 3.119.4 | MIT, Copyright (c) 2015-2016 Xamarin, Inc. and (c) 2017-2018 Microsoft Corporation | <https://github.com/mono/SkiaSharp> |
| Skia (the native library inside SkiaSharp's `libSkiaSharp`) | with SkiaSharp 3.119.4 | BSD-3-Clause, Copyright (c) 2011 Google Inc. | <https://skia.org/> |
| HarfBuzzSharp (`HarfBuzzSharp`, `HarfBuzzSharp.NativeAssets.Win32` / `.Linux`) | 8.3.1.3 | MIT, Copyright (c) 2015-2016 Xamarin, Inc. and (c) 2017-2018 Microsoft Corporation | <https://github.com/mono/SkiaSharp> |
| HarfBuzz (the native library inside `libHarfBuzzSharp`) | with HarfBuzzSharp 8.3.1.3 | "Old MIT", Copyright (c) Google, Mozilla, Red Hat, Behdad Esfahbod and others | <https://github.com/harfbuzz/harfbuzz> |
| ANGLE (`Avalonia.Angle.Windows.Natives`, `av_libglesv2.dll`, Windows) | 2.1.27548.20260419 | BSD-3-Clause, Copyright 2018 The ANGLE Project Authors | <https://github.com/AvaloniaUI/angle> (Avalonia's build of Google's ANGLE) |
| MicroCom.Runtime | 0.11.6 | MIT, Copyright 2021 Nikita Tsukanov | <https://github.com/kekekeks/MicroCom> |
| Tmds.DBus.Protocol (used on Linux; the DLL is in both builds) | 0.94.1 | MIT, Copyright Tom Deseyn | <https://github.com/tmds/Tmds.DBus> |
| Inter (the launcher's text font on Linux; bundled in `Avalonia.Fonts.Inter`, so also inside the Windows `FrightWorks.exe`) | 3.019 | SIL Open Font License 1.1, Copyright 2020 The Inter Project Authors ("Inter" is a trademark of Rasmus Andersson); the package `Avalonia.Fonts.Inter` 12.1.5 itself is MIT | <https://github.com/rsms/inter> |

`Avalonia.BuildServices` (a build-time package from Avalonia that reports usage) is not used: the
launcher and layout-test projects pin it with every asset excluded and the repository's
`Directory.Build.targets` empties its task, so nothing of it is built in or runs.

The native SkiaSharp and HarfBuzzSharp libraries are built from Skia, HarfBuzz and their own
dependencies (libpng, zlib, FreeType, ICU, expat, libjpeg-turbo, libwebp, and others); their copyright
notices and licenses are listed in full in the `THIRD-PARTY-NOTICES.txt` that comes with the
`SkiaSharp.NativeAssets.*` and `HarfBuzzSharp.NativeAssets.*` packages (version above).

**MIT** (all the MIT components above, each with its own copyright line from the table):

> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
> associated documentation files (the "Software"), to deal in the Software without restriction,
> including without limitation the rights to use, copy, modify, merge, publish, distribute,
> sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
> NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
> OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**BSD-3-Clause** (Skia, Copyright (c) 2011 Google Inc.; ANGLE, Copyright 2018 The ANGLE Project
Authors):

> Redistribution and use in source and binary forms, with or without modification, are permitted
> provided that the following conditions are met:
>
> - Redistributions of source code must retain the above copyright notice, this list of conditions
>   and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright notice, this list of
>   conditions and the following disclaimer in the documentation and/or other materials provided
>   with the distribution.
> - Neither the name of Google Inc. (Skia), nor of TransGaming Inc., Google Inc., 3DLabs Inc. Ltd.
>   (ANGLE), nor the names of their contributors may be used to endorse or promote products derived
>   from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
> IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
> FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
> CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
> DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
> DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
> IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
> OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

**Inter** - Copyright 2020 The Inter Project Authors (<https://github.com/rsms/inter>). "Inter" is a
trademark of Rasmus Andersson. This Font Software is licensed under the SIL Open Font License,
Version 1.1; the license, also available with a FAQ at <https://openfontlicense.org>, follows. The
launcher uses Inter unmodified.

```text
-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION AND CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Bundled in the setup executable's toolkit payload

The distributed `WiiCompiled-Setup.exe` carries a build toolkit so that translation and
compilation can run on a machine with nothing preinstalled. These tools are redistributed
unmodified, with their license texts, in the installer's `licenses/` folder.

| Component | License | Upstream |
| --- | --- | --- |
| llvm-mingw (Clang, LLD, libc++, libunwind, MinGW-w64 runtime) | Apache-2.0 with LLVM Exception; MinGW-w64 runtime under its own permissive terms; bundled GNU utilities under GPL-2.0-or-later or GPL-3.0-or-later | <https://github.com/mstorsjo/llvm-mingw> |
| CMake | BSD-3-Clause | <https://cmake.org/> |
| Ninja | Apache-2.0 | <https://ninja-build.org/> |
| nodtool (disc image extraction) | MIT OR Apache-2.0 | <https://github.com/encounter/nod> |
| Microsoft Visual C++ Runtime (`vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`) | Microsoft redistributable terms | Microsoft Visual Studio |
| `dxil.dll` | Microsoft redistributable (proprietary signing library) | Microsoft |

> [!IMPORTANT]
> The GNU utilities bundled inside llvm-mingw are GPL-licensed. Their complete corresponding source
> is available from the upstream project linked above at its pinned version, and this project will
> supply it on request for the exact version shipped in any given release. Pins live in
> `Launcher/Prepare-PortableTools.ps1` and `Launcher/NativeBuildFlags.ps1`.

---

## Development-only dependencies

Not redistributed in any release artifact.

| Component | License |
| --- | --- |
| xUnit.net 2.4.2, xunit.runner.visualstudio 2.4.5 | Apache-2.0 |
| Microsoft.NET.Test.Sdk 17.6.0 | MIT |
| coverlet.collector 6.0.0 | MIT |
| .NET 10 SDK | MIT |

---

## Reference material

Not code, but the documentation this project depends on:

- [WiiBrew](https://wiibrew.org/wiki/) - Wii hardware and IOS documentation.
- [Custom Mario Kart Wiiki (Tockdom)](https://wiki.tockdom.com/) - Mario Kart Wii file formats and
  modding documentation.
- [Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind) by ZPL - the mod distribution this
  project can build as a static profile. No Retro Rewind content is redistributed here; users
  supply their own copy.

---

If you believe a component is missing or misattributed here, please open an issue.
