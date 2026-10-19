#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Run Ghidra's analyzeHeadless with this repository's scripts on PATH and all of
# Ghidra's user state (config, cache, temp files, lock) kept under work/ghidra/.
#
#   tools/ghidra/headless.sh <project_dir> <project_name> [analyzeHeadless options...]
#
# Reads tools/local.env (git-ignored) for DQ8_GHIDRA_INSTALL_DIR and the optional
# DQ8_JAVA_HOME. DQ8_EXTRA_SCRIPT_PATH (';'-separated) adds third-party script
# directories, for example PS2Recomp's tools/ghidra. GHIDRA_HEADLESS_MAXMEM
# defaults to 8G. The working directory is the repository root, so relative paths
# given to scripts and .properties files resolve from there. Never edits the
# Ghidra install. Contributor tool only; never on the end-user setup path.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
if [ $# -lt 2 ]; then
    echo "usage: $0 <project_dir> <project_name> [analyzeHeadless options...]" >&2
    exit 2
fi
project_dir=$(realpath -m -- "$1")
project_name=$2
shift 2

if [ -f "$repo/tools/local.env" ]; then
    set -a
    # shellcheck disable=SC1091
    . "$repo/tools/local.env"
    set +a
fi
: "${DQ8_GHIDRA_INSTALL_DIR:?set DQ8_GHIDRA_INSTALL_DIR in tools/local.env}"
headless="$DQ8_GHIDRA_INSTALL_DIR/support/analyzeHeadless"
if [ ! -x "$headless" ]; then
    echo "error: $headless is not executable; check DQ8_GHIDRA_INSTALL_DIR" >&2
    exit 1
fi

state="$repo/work/ghidra"
mkdir -p "$state/config" "$state/cache" "$state/tmp" "$project_dir"
export XDG_CONFIG_HOME="$state/config"
export XDG_CACHE_HOME="$state/cache"
export JAVA_TOOL_OPTIONS="-Dapplication.tempdir=\"$state/tmp\"${JAVA_TOOL_OPTIONS:+ $JAVA_TOOL_OPTIONS}"
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"
if [ -n "${DQ8_JAVA_HOME:-}" ]; then
    export PATH="$DQ8_JAVA_HOME/bin:$PATH"
    export JAVA_HOME="$DQ8_JAVA_HOME"
fi

script_path="$repo/tools/ghidra"
if [ -n "${DQ8_EXTRA_SCRIPT_PATH:-}" ]; then
    script_path="$script_path;$DQ8_EXTRA_SCRIPT_PATH"
fi

cd "$repo"
exec flock "$state/headless.lock" "$headless" "$project_dir" "$project_name" -scriptPath "$script_path" "$@"
