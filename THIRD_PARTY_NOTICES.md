# Third-party notices

This repository redistributes prebuilt binaries under `prebuilt/` and vendors third-party sources under
`src/vendor/`. Some of them contain third-party software. The license texts below are copied unchanged from
the corresponding upstream license files (the same files are kept next to the vendored sources).

The code of this repository itself (the tool in `src/dc_determinism/`, the workers in `src/worker/`, the scripts, and
the patches in `src/worker/pybind/patches/`) is licensed under the MIT License (see `LICENSE`). This file reproduces the
notices of the third-party components.

## Which binaries and sources contain which components

| Binary or source | Contains |
| --- | --- |
| `prebuilt/worker/windows-amd64/dc_worker.exe`, `prebuilt/worker/linux-amd64/dc_worker` | None of the components below except the runtime libraries linked statically (see "Compiler runtime libraries"; the Windows `dc_worker.exe` also embeds winpthreads, see "winpthreads"). `dc_worker` loads the DigitalCurling simulator plugin loader dynamically at run time and does not link any of its code. |
| `prebuilt/loaders/**` (plugin loader libraries `libdigitalcurling_plugin_loader.dll` / `.so` / `digitalcurling_plugin_loader.dll`) | DigitalCurling simulator library (revision 762eef67560cd83b23ef3afd272ac45e1ff28d7b), fpm (commit b46537fe9697e1a598ac8a26f8ae43d8b286ac3f), Box2D (commit 9ebbbcd960ad424e03e5de6e66a40764c16f51bc), nlohmann/json (commit 55f93686c01528224f448c19128836e7df245f72) and xxHash (commit e626a72bc2321cd320e953a0ccf1584cad60f363). The Windows gcc and clang bundles also ship the MSYS2 MinGW runtime DLLs `libgcc_s_seh-1.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` (see "MSYS2 MinGW runtime DLLs"). |
| `prebuilt/players/**` (`normal_dist` and `nd2` player binaries for msvc, gcc, clang and WebAssembly; `worker`, `worker.exe`, `worker.js`, `worker.wasm`) | DigitalCurling player library (revision 762eef67560cd83b23ef3afd272ac45e1ff28d7b), fpm (commit b46537fe9697e1a598ac8a26f8ae43d8b286ac3f) and nlohmann/json (commit 55f93686c01528224f448c19128836e7df245f72). |
| `src/vendor/` (the sources `dc_determinism prebuild` builds the loaders and players from: `digitalcurling/`, `fpm/`, `box2d/`, `nlohmann-json/`, `xxhash/`, `miniz/`) | The components of the rows above, as sources, each with its upstream license file next to it. Pinned revisions, Git tree ids and tree hashes: `src/vendor/manifest.json`. |
| `src/dc_determinism/**` and the `prebuilt/dc_determinism/windows-amd64/dc_determinism.exe` and `prebuilt/dc_determinism/linux-amd64/dc_determinism` binaries built from it (C++ implementation of the tool) | miniz 3.1.2 (`src/vendor/miniz`, ZIP reading and writing) and nlohmann/json (commit 55f93686c01528224f448c19128836e7df245f72, `src/vendor/nlohmann-json`), plus the runtime libraries linked statically (see "Compiler runtime libraries"). The Linux binary is linked with `-static` and therefore contains the GNU C Library (glibc, see "GNU C Library (glibc)"). None of the loader components above. |
| `prebuilt/pybind/<engine>/simulator-official.so` (`fcv1-py27`, `fcv1-py36`; official pybind extensions of FCV1-Simulator, copied unchanged and checked against the SHA-256 in `prebuilt/pybind/manifest.json`) | Built by the FCV1-Simulator project, not by this repository: FCV1-Simulator (the versions `prebuilt/pybind/manifest.json` records for the two engines are commits dc1bfeecb41c7685f68f8b9d71bf7e7009362e5c (v1.0.27) and 4a567c2cd46bc1ff76ae98c6dff9b491b1dd3b98 (v1.0.36)), and, linked into the extension, Box2D, nlohmann/json and pybind11. The versions of Box2D, nlohmann/json and pybind11 inside the official binaries are not recorded: the upstream `requirements` files list `pybind11` without a version (and `numpy < 2.0`). |
| `prebuilt/pybind/<engine>/simulator-record-v2.so` (recording build of the same two FCV1-Simulator commits, built from this repository's recipe `src/worker/pybind/container-build.sh`) | FCV1-Simulator at the commits above, Box2D (commit 9ebbbcd960ad424e03e5de6e66a40764c16f51bc), nlohmann/json (commit 11a835df85677002a8aadc5b4e945684c5b7f68b) and pybind11 3.1.0 (pip), plus the patches `src/worker/pybind/patches/fcv1-py27-record-v2.patch` and `fcv1-py36-record-v2.patch`, which are part of this repository (MIT License, see above). |
| `prebuilt/worker/wasm/**` (`dc_worker.js`, `dc_worker.wasm`) and the WebAssembly players | The loader components and, in addition, the Emscripten runtime (see "WebAssembly simulator worker"). |

**linux-arm64:** the linux-arm64 binaries (`prebuilt/dc_determinism/linux-arm64/dc_determinism`, `prebuilt/worker/linux-arm64/dc_worker`, `prebuilt/loaders/linux-arm64/**`, `prebuilt/players/linux-arm64-*/**`) are not in this repository at present; they are built on an arm64 host (see the README). Once built, they contain the same components as their linux-amd64 counterparts (and the same notices apply).

The pinned revisions, Git tree ids and include-tree hashes of the prebuilt players are recorded in
`prebuilt/manifest.json` (section `players.source`); those of the prebuilt loaders in
`prebuilt/loaders/*/manifest.json`; those of the pybind builds in `prebuilt/pybind/manifest.json`.

## DigitalCurling (simulator library and player library)

Source: `src/vendor/digitalcurling/LICENSE`. One vendored tree (revision 762eef67560cd83b23ef3afd272ac45e1ff28d7b) with one license file is the source of both the simulator library (in the loaders) and the player library (in the players).

```text
MIT License

Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## fpm (fixed-point math library)

Source: `LICENSE` of the fpm work tree at commit b46537fe9697e1a598ac8a26f8ae43d8b286ac3f (players and loaders; `src/vendor/fpm/LICENSE`, same text)

```text
MIT License

Copyright (c) 2019 Mike Lankamp

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## nlohmann/json (JSON for Modern C++)

Source: `LICENSE.MIT` of the nlohmann/json work tree at commit 55f93686c01528224f448c19128836e7df245f72 (`src/vendor/nlohmann-json/LICENSE.MIT`); the `LICENSE.MIT` of commit 11a835df85677002a8aadc5b4e945684c5b7f68b (the version inside the recording pybind binaries) is the same text

```text
MIT License 

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Box2D

Source: `src/vendor/box2d/LICENSE` (commit 9ebbbcd960ad424e03e5de6e66a40764c16f51bc, also the commit of the recording pybind binaries)

```text
MIT License

Copyright (c) 2019 Erin Catto

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## xxHash

Source: `src/vendor/xxhash/LICENSE` (commit e626a72bc2321cd320e953a0ccf1584cad60f363; the library files, which are BSD 2-Clause licensed, are the only part used)

```text
xxHash Library
Copyright (c) 2012-2021 Yann Collet
All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## miniz

Source: `src/vendor/miniz/LICENSE` (release 3.1.2, commit 77d0dce8627735138c51770d1799a1ef48f2117d; the single-file amalgamation `miniz.c` / `miniz.h` is the only part used)

```text
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## FCV1-Simulator (pybind binaries)

Source: `LICENSE` of github.com/kr-work/FCV1-Simulator at commit dc1bfeecb41c7685f68f8b9d71bf7e7009362e5c (v1.0.27) and at commit 4a567c2cd46bc1ff76ae98c6dff9b491b1dd3b98 (v1.0.36); the two files are identical. Applies to `prebuilt/pybind/**` (the official `simulator-official.so` and the `simulator-record-v2.so` built from the same sources).

```text
MIT License

Copyright (c) 2023 Being24

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## pybind11 (pybind binaries)

Source: `LICENSE` of pybind11 v3.1.0 (commit 97bf890), the version installed by the recording build (`pip install pybind11==3.1.0`; BSD-3-Clause). The pybind11 version inside the official `simulator-official.so` is not recorded (the upstream `requirements` files do not pin it); only the v3.1.0 text was fetched and is reproduced here.

```text
Copyright (c) 2016 Wenzel Jakob <wenzel.jakob@epfl.ch>, All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Please also refer to the file .github/CONTRIBUTING.md, which clarifies licensing of
external contributions to this project including patches, pull requests, etc.
```

## Compiler runtime libraries (GCC and libstdc++)

The binaries built with GCC or clang link the GCC runtime libraries (libgcc, libstdc++) statically, as recorded in the link flags of prebuilt/manifest.json: the Windows `dc_worker.exe` and the Windows gcc and clang players (`-static`, MSYS2 libstdc++), the Linux `dc_worker` and the Linux gcc and clang players (`-static-libstdc++ -static-libgcc`, Ubuntu 24.04 GCC 13.3), and the Linux `dc_determinism` (`-static`). libgcc and libstdc++ are licensed under the GNU General Public License version 3 with the GCC Runtime Library Exception, version 3.1 (reproduced below). The exception permits conveying a work of target code that combines these runtime libraries with independent modules under terms of your choice. The GPLv3 text is available at https://www.gnu.org/licenses/gpl-3.0.txt and as the file `COPYING3` of the GCC sources and of the MSYS2 `gcc` package. The `msvc` binaries link the Microsoft Visual C++ runtime statically (`/MT`); its terms are Microsoft's and are not reproduced here. The Windows binaries linked with `-static` (`dc_worker.exe` and the gcc and clang players' `worker.exe`) also embed winpthreads (see "winpthreads"). The Linux `-static` binary contains glibc (next section).

## GNU C Library (glibc)

`prebuilt/dc_determinism/linux-amd64/dc_determinism` is linked with `-static` (`link_flags` `-static`, `strip -s` in `prebuilt/manifest.json`) in the `dc_determinism-cpu:local` image (Ubuntu 24.04), so it contains parts of the GNU C Library of the image's package `libc6` 2.39-0ubuntu8.12. The C library is licensed under the GNU Lesser General Public License, version 2.1 or (at your option) any later version. The complete LGPL-2.1 text is the file `/usr/share/common-licenses/LGPL-2.1` of the image (SHA-256 dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551) and https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt; it is referenced here, not reproduced. The glibc source of the package is published by Ubuntu (source package `glibc`, https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/) and by the GNU project (https://sourceware.org/git/glibc.git); these locations were not accessed while preparing this file. The LGPL also requires that recipients can relink the work with a modified library; this repository ships the sources of `dc_determinism` (`src/dc_determinism`) and the build recipe (`scripts/prebuild.sh`). The notice below is copied unchanged from `/usr/share/doc/libc6/copyright` of the image (the first part of the file; its text says "version 2.23" although the package version is 2.39, which is an outdated statement in the Ubuntu file):

```text
This is the Debian prepackaged version of the GNU C Library version 2.23.

It was put together by the GNU Libc Maintainers <debian-glibc@lists.debian.org>
from <https://sourceware.org/git/glibc.git>

* Most of the GNU C library is under the following copyright:

    Copyright (C) 1991-2015 Free Software Foundation, Inc.

    The GNU C Library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    The GNU C Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with the GNU C Library; if not, write to the Free
    Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
    02110-1301 USA
```
## MSYS2 MinGW runtime DLLs (`prebuilt/loaders/windows-amd64/{gcc,clang}/`)

The Windows gcc and clang loader bundles ship three runtime DLLs of the MSYS2 UCRT64 environment next to `libdigitalcurling_plugin_loader.dll` (the same three files in both bundles). Package names and versions are those recorded in `toolchain.windows-amd64.gcc` of `prebuilt/manifest.json`:

| DLL | MSYS2 package (version) | Upstream | License | SHA-256 |
| --- | --- | --- | --- | --- |
| `libgcc_s_seh-1.dll` | `mingw-w64-ucrt-x86_64-libgcc` 16.2.0-4 | GCC 16.2.0 (libgcc) | GPLv3 with the GCC Runtime Library Exception 3.1 | 68ca4574b331402879dec403ba4f14491c9b8fc4aa3f4a41432008714b9bc5df |
| `libstdc++-6.dll` | `mingw-w64-ucrt-x86_64-libstdc++` 16.2.0-4 | GCC 16.2.0 (libstdc++-v3) | GPLv3 with the GCC Runtime Library Exception 3.1 | b30dbd914d903992acb28b38455947153886656beafbd91c465c31aa258f9c45 |
| `libwinpthread-1.dll` | `mingw-w64-ucrt-x86_64-libwinpthread` 14.0.0.r426.g4564ee4b5-1 (built from the `winpthreads` package of the same version) | mingw-w64 winpthreads | MIT-style license of the mingw-w64 project (with parts under a BSD-3-Clause license of Lockless Inc.), see below | 76cc52adfcdb8c800813b2ae8da42a908934eb1ee54ce8e9744fc8d703c76ee1 |

Where the corresponding sources are published (these locations were not accessed while preparing this file):

- The MSYS2 source packages of the exact versions above, for `mingw-w64-gcc` (the package that builds `libgcc` and `libstdc++`) and for `mingw-w64-winpthreads`, in the MSYS2 package repository (https://github.com/msys2/MINGW-packages holds the build recipes; the source archives are under https://repo.msys2.org/mingw/sources/).
- GCC (including libgcc and libstdc++-v3) upstream: https://gcc.gnu.org/ (release 16.2.0 sources at https://ftp.gnu.org/gnu/gcc/).
- mingw-w64 (crt, headers, winpthreads) upstream: https://www.mingw-w64.org/ (source repository https://sourceforge.net/p/mingw-w64/mingw-w64/).

The license texts below are copied unchanged from the MSYS2 package files `C:\msys64\ucrt64\share\licenses\libgcc\COPYING.RUNTIME` (identical to `gcc\COPYING.RUNTIME`) and `...\licenses\libwinpthread\COPYING` (identical to `winpthreads\COPYING`). The GPLv3 text itself (`COPYING3` of the same packages) is referenced, not reproduced: https://www.gnu.org/licenses/gpl-3.0.txt.

### GCC Runtime Library Exception 3.1 (libgcc_s_seh-1.dll, libstdc++-6.dll)

Source: `COPYING.RUNTIME`

```text
GCC RUNTIME LIBRARY EXCEPTION

Version 3.1, 31 March 2009

Copyright (C) 2009 Free Software Foundation, Inc. <http://fsf.org/>

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.

This GCC Runtime Library Exception ("Exception") is an additional
permission under section 7 of the GNU General Public License, version
3 ("GPLv3"). It applies to a given file (the "Runtime Library") that
bears a notice placed by the copyright holder of the file stating that
the file is governed by GPLv3 along with this Exception.

When you use GCC to compile a program, GCC may combine portions of
certain GCC header files and runtime libraries with the compiled
program. The purpose of this Exception is to allow compilation of
non-GPL (including proprietary) programs to use, in this way, the
header files and runtime libraries covered by this Exception.

0. Definitions.

A file is an "Independent Module" if it either requires the Runtime
Library for execution after a Compilation Process, or makes use of an
interface provided by the Runtime Library, but is not otherwise based
on the Runtime Library.

"GCC" means a version of the GNU Compiler Collection, with or without
modifications, governed by version 3 (or a specified later version) of
the GNU General Public License (GPL) with the option of using any
subsequent versions published by the FSF.

"GPL-compatible Software" is software whose conditions of propagation,
modification and use would permit combination with GCC in accord with
the license of GCC.

"Target Code" refers to output from any compiler for a real or virtual
target processor architecture, in executable form or suitable for
input to an assembler, loader, linker and/or execution
phase. Notwithstanding that, Target Code does not include data in any
format that is used as a compiler intermediate representation, or used
for producing a compiler intermediate representation.

The "Compilation Process" transforms code entirely represented in
non-intermediate languages designed for human-written code, and/or in
Java Virtual Machine byte code, into Target Code. Thus, for example,
use of source code generators and preprocessors need not be considered
part of the Compilation Process, since the Compilation Process can be
understood as starting with the output of the generators or
preprocessors.

A Compilation Process is "Eligible" if it is done using GCC, alone or
with other GPL-compatible software, or if it is done without using any
work based on GCC. For example, using non-GPL-compatible Software to
optimize any GCC intermediate representations would not qualify as an
Eligible Compilation Process.

1. Grant of Additional Permission.

You have permission to propagate a work of Target Code formed by
combining the Runtime Library with Independent Modules, even if such
propagation would otherwise violate the terms of GPLv3, provided that
all Target Code was generated by Eligible Compilation Processes. You
may then convey such a combination under terms of your choice,
consistent with the licensing of the Independent Modules.

2. No Weakening of GCC Copyleft.

The availability of this Exception does not imply any general
presumption that third-party software is unaffected by the copyleft
requirements of the license of GCC.
```

### winpthreads (libwinpthread-1.dll)

Source: `COPYING` of the `winpthreads` package. Applies to `libwinpthread-1.dll` and also to the winpthreads code statically embedded in `prebuilt/worker/windows-amd64/dc_worker.exe` and in the `worker.exe` of the Windows gcc and clang players (`prebuilt/players/windows-amd64-{gcc,clang}/*/worker.exe`); the mingw-w64 winpthreads source paths appear in the strings of these files.

```text
Copyright (c) 2011 mingw-w64 project

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.


/*
 * Parts of this library are derived by:
 *
 * Posix Threads library for Microsoft Windows
 *
 * Use at own risk, there is no implied warranty to this code.
 * It uses undocumented features of Microsoft Windows that can change
 * at any time in the future.
 *
 * (C) 2010 Lockless Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of Lockless Inc. nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AN
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
```

## WebAssembly simulator worker (`prebuilt/worker/wasm/`)

`dc_worker.js` and `dc_worker.wasm` (`dc_determinism prebuild`, `src/worker/wasm/`) contain the same
components as the prebuilt loaders: the DigitalCurling simulator library (plugin loader and the fcv1 and fcv1plus
simulator plugins, linked statically), fpm, Box2D, nlohmann/json and xxHash; their license texts are the
sections above. In addition the module contains the Emscripten 6.0.9 runtime libraries that were linked into it from
the `emscripten/emsdk:6.0.9` image (`-lc`, `-lc++-legacyexcept`, `-lc++abi-legacyexcept`, `-lunwind-legacyexcept`,
`-lclang_rt.builtins-legacysjlj`, `-ldlmalloc`, and the small stub libraries `-lstubs`, `-lsockets`, `-lhtml5`, `-lal`,
`-lGL-getprocaddr`), and `dc_worker.js` is the Emscripten JavaScript runtime. The license texts below are copied
unchanged from the files of that image (`/emsdk/upstream/emscripten/LICENSE`, `system/lib/libcxx/LICENSE.TXT`,
`system/lib/libc/musl/COPYRIGHT`, the header of `system/lib/dlmalloc.c`).

### Emscripten (JavaScript runtime and system libraries)

Source: `/emsdk/upstream/emscripten/LICENSE`
```text
Emscripten is available under 2 licenses, the MIT license and the
University of Illinois/NCSA Open Source License.

Both are permissive open source licenses, with little if any
practical difference between them.

The reason for offering both is that (1) the MIT license is
well-known, while (2) the University of Illinois/NCSA Open Source
License allows Emscripten's code to be integrated upstream into
LLVM, which uses that license, should the opportunity arise.

The full text of both licenses follows.

==============================================================================

Copyright (c) 2010-2014 Emscripten authors, see AUTHORS file.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

==============================================================================

Copyright (c) 2010-2014 Emscripten authors, see AUTHORS file.
All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal with the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

    Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimers.

    Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimers
    in the documentation and/or other materials provided with the
    distribution.

    Neither the names of Mozilla,
    nor the names of its contributors may be used to endorse
    or promote products derived from this Software without specific prior
    written permission. 

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE SOFTWARE.

==============================================================================

This program uses portions of Node.js source code located in src/library_path.js,
in accordance with the terms of the MIT license. Node's license follows:

    """
        Copyright Joyent, Inc. and other Node contributors. All rights reserved.
        Permission is hereby granted, free of charge, to any person obtaining a copy
        of this software and associated documentation files (the "Software"), to
        deal in the Software without restriction, including without limitation the
        rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
        sell copies of the Software, and to permit persons to whom the Software is
        furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be included in
        all copies or substantial portions of the Software.

        THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
        IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
        FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
        AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
        LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
        FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
        IN THE SOFTWARE.
    """

The musl libc project is bundled in this repo, and it has the MIT license, see
system/lib/libc/musl/COPYRIGHT

The third_party/ subdirectory contains code with other licenses. None of it is
used by default, but certain options use it (e.g., the optional closure compiler
flag will run closure compiler from third_party/).
```

### LLVM libc++, libc++abi, libunwind and compiler-rt

Source: `/emsdk/upstream/emscripten/system/lib/libcxx/LICENSE.TXT`. The LICENSE.TXT files of libc++abi, libunwind and
compiler-rt in the same image are identical except for the name of the library in the sentence that begins "The ...
library is dual licensed" and, for compiler-rt, the year in the CREDITS.TXT line (2009-2015).
```text
==============================================================================
The LLVM Project is under the Apache License v2.0 with LLVM Exceptions:
==============================================================================

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

    TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

    1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

    2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

    3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

    4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

    5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

    6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

    7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

    8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

    9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

    END OF TERMS AND CONDITIONS

    APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

    Copyright [yyyy] [name of copyright owner]

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.


---- LLVM Exceptions to the Apache 2.0 License ----

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into an Object form of such source code, you
may redistribute such embedded portions in such Object form without complying
with the conditions of Sections 4(a), 4(b) and 4(d) of the License.

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 ("Combined Software") and if a
court of competent jurisdiction determines that the patent provision (Section
3), the indemnity provision (Section 9) or other Section of the License
conflicts with the conditions of the GPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of
the License, but only in their entirety and only with respect to the Combined
Software.

==============================================================================
Software from third parties included in the LLVM Project:
==============================================================================
The LLVM Project contains third party software which is under different license
terms. All such code will be identified clearly using at least one of two
mechanisms:
1) It will be in a separate directory tree with its own `LICENSE.txt` or
   `LICENSE` file at the top containing the specific license and restrictions
   which apply to that software, or
2) It will contain specific license and restriction terms at the top of every
   file.

==============================================================================
Legacy LLVM License (https://llvm.org/docs/DeveloperPolicy.html#legacy):
==============================================================================

The libc++ library is dual licensed under both the University of Illinois
"BSD-Like" license and the MIT license.  As a user of this code you may choose
to use it under either license.  As a contributor, you agree to allow your code
to be used under both.

Full text of the relevant licenses is included below.

==============================================================================

University of Illinois/NCSA
Open Source License

Copyright (c) 2009-2019 by the contributors listed in CREDITS.TXT

All rights reserved.

Developed by:

    LLVM Team

    University of Illinois at Urbana-Champaign

    http://llvm.org

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal with
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimers.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimers in the
      documentation and/or other materials provided with the distribution.

    * Neither the names of the LLVM Team, University of Illinois at
      Urbana-Champaign, nor the names of its contributors may be used to
      endorse or promote products derived from this Software without specific
      prior written permission.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE
SOFTWARE.

==============================================================================

Copyright (c) 2009-2014 by the contributors listed in CREDITS.TXT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### musl libc

Source: `/emsdk/upstream/emscripten/system/lib/libc/musl/COPYRIGHT`
```text
musl as a whole is licensed under the following standard MIT license:

----------------------------------------------------------------------
Copyright © 2005-2020 Rich Felker, et al.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
----------------------------------------------------------------------

Authors/contributors include:

A. Wilcox
Ada Worcester
Alex Dowad
Alex Suykov
Alexander Monakov
Andre McCurdy
Andrew Kelley
Anthony G. Basile
Aric Belsito
Arvid Picciani
Bartosz Brachaczek
Benjamin Peterson
Bobby Bingham
Boris Brezillon
Brent Cook
Chris Spiegel
Clément Vasseur
Daniel Micay
Daniel Sabogal
Daurnimator
David Carlier
David Edelsohn
Denys Vlasenko
Dmitry Ivanov
Dmitry V. Levin
Drew DeVault
Emil Renner Berthing
Fangrui Song
Felix Fietkau
Felix Janda
Gianluca Anzolin
Hauke Mehrtens
He X
Hiltjo Posthuma
Isaac Dunham
Jaydeep Patil
Jens Gustedt
Jeremy Huntwork
Jo-Philipp Wich
Joakim Sindholt
John Spencer
Julien Ramseier
Justin Cormack
Kaarle Ritvanen
Khem Raj
Kylie McClain
Leah Neukirchen
Luca Barbato
Luka Perkov
Lynn Ochs
M Farkas-Dyck (Strake)
Mahesh Bodapati
Markus Wichmann
Masanori Ogino
Michael Clark
Michael Forney
Mikhail Kremnyov
Natanael Copa
Nicholas J. Kain
orc
Pascal Cuoq
Patrick Oppenlander
Petr Hosek
Petr Skocik
Pierre Carrier
Reini Urban
Rich Felker
Richard Pennington
Ryan Fairfax
Samuel Holland
Segev Finer
Shiz
sin
Solar Designer
Stefan Kristiansson
Stefan O'Rear
Szabolcs Nagy
Timo Teräs
Trutz Behn
Will Dietz
William Haddon
William Pitcock

Portions of this software are derived from third-party works licensed
under terms compatible with the above MIT license:

The TRE regular expression implementation (src/regex/reg* and
src/regex/tre*) is Copyright © 2001-2008 Ville Laurikari and licensed
under a 2-clause BSD license (license text in the source files). The
included version has been heavily modified by Rich Felker in 2012, in
the interests of size, simplicity, and namespace cleanliness.

Much of the math library code (src/math/* and src/complex/*) is
Copyright © 1993,2004 Sun Microsystems or
Copyright © 2003-2011 David Schultz or
Copyright © 2003-2009 Steven G. Kargl or
Copyright © 2003-2009 Bruce D. Evans or
Copyright © 2008 Stephen L. Moshier or
Copyright © 2017-2018 Arm Limited
and labelled as such in comments in the individual source files. All
have been licensed under extremely permissive terms.

The ARM memcpy code (src/string/arm/memcpy.S) is Copyright © 2008
The Android Open Source Project and is licensed under a two-clause BSD
license. It was taken from Bionic libc, used on Android.

The AArch64 memcpy and memset code (src/string/aarch64/*) are
Copyright © 1999-2019, Arm Limited.

The implementation of DES for crypt (src/crypt/crypt_des.c) is
Copyright © 1994 David Burren. It is licensed under a BSD license.

The implementation of blowfish crypt (src/crypt/crypt_blowfish.c) was
originally written by Solar Designer and placed into the public
domain. The code also comes with a fallback permissive license for use
in jurisdictions that may not recognize the public domain.

The smoothsort implementation (src/stdlib/qsort.c) is Copyright © 2011
Lynn Ochs and is licensed under an MIT-style license.

The x86_64 port was written by Nicholas J. Kain and is licensed under
the standard MIT terms.

The mips and microblaze ports were originally written by Richard
Pennington for use in the ellcc project. The original code was adapted
by Rich Felker for build system and code conventions during upstream
integration. It is licensed under the standard MIT terms.

The mips64 port was contributed by Imagination Technologies and is
licensed under the standard MIT terms.

The powerpc port was also originally written by Richard Pennington,
and later supplemented and integrated by John Spencer. It is licensed
under the standard MIT terms.

All other files which have no copyright comments are original works
produced specifically for use as part of this library, written either
by Rich Felker, the main author of the library, or by one or more
contibutors listed above. Details on authorship of individual files
can be found in the git version control history of the project. The
omission of copyright and license comments in each file is in the
interest of source tree size.

In addition, permission is hereby granted for all public header files
(include/* and arch/*/bits/*) and crt files intended to be linked into
applications (crt/*, ldso/dlstart.c, and arch/*/crt_arch.h) to omit
the copyright notice and permission notice otherwise required by the
license, and to use these files without any requirement of
attribution. These files include substantial contributions from:

Bobby Bingham
John Spencer
Nicholas J. Kain
Rich Felker
Richard Pennington
Stefan Kristiansson
Szabolcs Nagy

all of whom have explicitly granted such permission.

This file previously contained text expressing a belief that most of
the files covered by the above exception were sufficiently trivial not
to be subject to copyright, resulting in confusion over whether it
negated the permissions granted in the license. In the spirit of
permissive licensing, and of not having licensing issues being an
obstacle to adoption, that text has been removed.
```

### dlmalloc

Source: the header of `/emsdk/upstream/emscripten/system/lib/dlmalloc.c`
```text
/*
 This is a version (aka dlmalloc) of malloc/free/realloc written by
 Doug Lea and released to the public domain, as explained at
 http://creativecommons.org/publicdomain/zero/1.0/ Send questions,
 comments, complaints, performance data, etc to dl@cs.oswego.edu
 
 * Version 2.8.6 Wed Aug 29 06:57:58 2012  Doug Lea
 Note: There may be an updated version of this malloc obtainable at
 ftp://gee.cs.oswego.edu/pub/misc/malloc.c
 Check before installing!
*/
```
