#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# wine-run.sh WINE PROGRAM [ARG...]: runs a Windows program of the cross build under Wine (llvm-mingw.cmake's
# CMAKE_CROSSCOMPILING_EMULATOR). Arguments that are absolute paths on this machine become paths on Wine's Z:
# drive: Windows command lines read a leading '/' as an option (Catch2 does, so its test discovery's
# `--out /path` fails otherwise).
wine=$1
program=$2
shift 2
n=$#
while [ "$n" -gt 0 ]; do
    arg=$1
    shift
    case $arg in
        /*)
            if [ -e "$arg" ] || [ -d "$(dirname -- "$arg")" ]; then
                arg="Z:$arg"
            fi
            ;;
    esac
    set -- "$@" "$arg"
    n=$((n - 1))
done
exec "$wine" "$program" "$@"
