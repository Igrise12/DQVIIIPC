#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Contributor tool (M5 session E): builds Windows game modules from an existing recompile work directory with the
# Linux-hosted llvm-mingw, through cmake/module and cmake/toolchains/llvm-mingw.cmake, for smoke runs of the
# cross-built dq8-game.exe under Wine. Each unit's parameters are read from the Linux module build in the same
# work directory (work/recomp/build/<unit>/CMakeCache.txt, written by `dq8ctl recomp build`).
#
#   packaging/windows/cross-build-modules.sh WORK OUT [JOBS]
#     WORK   the recompile work directory (generated/ and build/ from `dq8ctl recomp all`)
#     OUT    receives build/<unit>/ and the modules (OUT/modules/dq8_unit_<unit>.dll)
#   LLVM_MINGW_ROOT must name the unpacked llvm-mingw (packaging/windows/fetch_tools.cmake).
#
# The output is derived from the user's disc: keep it local (work/ is git-ignored).
set -euo pipefail
work=${1:?usage: cross-build-modules.sh WORK OUT [JOBS]}
out=${2:?usage: cross-build-modules.sh WORK OUT [JOBS]}
jobs=${3:-$(nproc)}
: "${LLVM_MINGW_ROOT:?set LLVM_MINGW_ROOT to the unpacked llvm-mingw}"
repo=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$out/modules"

cache_value() { # cache_value FILE NAME
    sed -n "s/^$2:[A-Z]*=//p" "$1"
}

for cache in "$work"/build/*/CMakeCache.txt; do
    unit=$(cache_value "$cache" DQ8_UNIT_NAME)
    [ -n "$unit" ] || continue
    echo "== $unit"
    cmake -S "$repo/cmake/module" -B "$out/build/$unit" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$repo/cmake/toolchains/llvm-mingw.cmake" -DLLVM_MINGW_ROOT="$LLVM_MINGW_ROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DDQ8_UNIT_NAME="$unit" \
        -DDQ8_UNIT_KIND="$(cache_value "$cache" DQ8_UNIT_KIND)" \
        -DDQ8_UNIT_OVERLAY_ID="$(cache_value "$cache" DQ8_UNIT_OVERLAY_ID)" \
        -DDQ8_UNIT_GENERATED_DIR="$work/generated/$unit" \
        -DDQ8_UNIT_PROFILE_ID="$(cache_value "$cache" DQ8_UNIT_PROFILE_ID)" \
        -DDQ8_UNIT_SOURCE_SHA1="$(cache_value "$cache" DQ8_UNIT_SOURCE_SHA1)" \
        -DDQ8_GENERATOR="$(cache_value "$cache" DQ8_GENERATOR)" \
        -DDQ8_UNIT_ENTRY_POINT="$(cache_value "$cache" DQ8_UNIT_ENTRY_POINT)" \
        -DDQ8_ABI_INCLUDE_DIR="$repo/include" >/dev/null
    cmake --build "$out/build/$unit" --parallel "$jobs"
    cp "$out/build/$unit/module/dq8_unit_$unit.dll" "$out/modules/"
done
ls -l "$out/modules"
