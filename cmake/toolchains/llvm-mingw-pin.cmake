# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# The pinned Windows toolchain and build tools (ADR-0018), one place for the CI job, packaging/windows/ and the
# unit test that checks src/core/toolchain/download.cpp carries the same pin. Every file comes from the project's
# own GitHub release page; the SHA-256 values are the ones GitHub publishes for those assets, checked again on
# download. Bump all three together with a note in Docs/milestones/M5-launcher.md (or its successor).

# llvm-mingw (clang, lld, mingw-w64, libc++), UCRT flavour: Windows 10 and later.
set(DQ8_LLVM_MINGW_VERSION "20261020")
set(DQ8_LLVM_MINGW_LLVM_VERSION "23.1.4")
set(DQ8_LLVM_MINGW_RELEASE "https://github.com/mstorsjo/llvm-mingw/releases/download/${DQ8_LLVM_MINGW_VERSION}")
# Linux-hosted cross compiler: CI and contributors cross-building the host.
set(DQ8_LLVM_MINGW_LINUX_ARCHIVE "llvm-mingw-${DQ8_LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz")
set(DQ8_LLVM_MINGW_LINUX_SHA256 "55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335")
# Windows-hosted compiler: what the setup downloads for users (src/core/toolchain/download.cpp).
set(DQ8_LLVM_MINGW_WINDOWS_ARCHIVE "llvm-mingw-${DQ8_LLVM_MINGW_VERSION}-ucrt-x86_64.zip")
set(DQ8_LLVM_MINGW_WINDOWS_SHA256 "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd")
set(DQ8_LLVM_MINGW_WINDOWS_SIZE "190612575")

# CMake and Ninja for Windows, shipped inside the zip (packaging/windows/).
set(DQ8_WINDOWS_CMAKE_VERSION "4.4.4")
set(DQ8_WINDOWS_CMAKE_ARCHIVE "cmake-${DQ8_WINDOWS_CMAKE_VERSION}-windows-x86_64.zip")
set(DQ8_WINDOWS_CMAKE_URL
    "https://github.com/Kitware/CMake/releases/download/v${DQ8_WINDOWS_CMAKE_VERSION}/${DQ8_WINDOWS_CMAKE_ARCHIVE}")
set(DQ8_WINDOWS_CMAKE_SHA256 "bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536")
set(DQ8_WINDOWS_NINJA_VERSION "1.13.2")
set(DQ8_WINDOWS_NINJA_URL
    "https://github.com/ninja-build/ninja/releases/download/v${DQ8_WINDOWS_NINJA_VERSION}/ninja-win.zip")
set(DQ8_WINDOWS_NINJA_SHA256 "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65")
# ninja-win.zip holds only ninja.exe: its licence (Apache-2.0) comes from the release's tag in the same repository.
set(DQ8_WINDOWS_NINJA_COPYING_URL
    "https://raw.githubusercontent.com/ninja-build/ninja/v${DQ8_WINDOWS_NINJA_VERSION}/COPYING")
set(DQ8_WINDOWS_NINJA_COPYING_SHA256 "eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea")
