DQ8-Native for Windows (portable)
=================================

DQ8-Native is a native PC port of Dragon Quest VIII (PS2, NTSC-U). It contains no game data: you need your own
copy of the disc as an ISO image.

Start dq8-launcher.exe. The first run asks for your ISO, checks it, and builds the game code from it on this
computer. That build needs a C++ compiler: the launcher offers to download the pinned llvm-mingw toolchain
(about 190 MB, unpacked to about 750 MB in %APPDATA%\DQ8-Native\toolchain) and asks before it does. The
first build takes 15 to 40 minutes; you can cancel it and resume later.

Requirements: 64-bit Windows 10 (version 1903 or later) or Windows 11, a Vulkan 1.3 graphics driver, about 3 GB
of free disk space for the toolchain and the build.

Everything this program writes goes to %APPDATA%\DQ8-Native (settings, memory cards, the toolchain, the build).
Delete that folder and this one to remove it.

Licences: DQ8-Native is free software under the GNU General Public License, version 3 or later (LICENSE.txt).
Third-party components and their licences are listed in THIRD_PARTY_NOTICES.md and LICENSES\; dq8-gs-parallel.dll
is under the LGPL (dq8-gs-parallel.COPYING.LGPLv3, dq8-gs-parallel.NOTICE.md); tools\cmake and tools\ninja are
CMake (BSD-3-Clause, tools\cmake\doc\cmake) and Ninja (Apache-2.0, tools\ninja\COPYING).
