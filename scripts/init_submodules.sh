#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Initialises the pinned submodules without recursing into parallel-gs's full Granite tree. Granite pulls more
# than a dozen nested repositories (the SPIR-V toolchain, FSR 2, SDL, ...) that only paraLLEl-GS's own tools
# need; the library we build needs Granite, volk and Vulkan-Headers (Docs/spikes/S5-parallel-gs-standalone.md,
# the same subset as upstream's checkout_submodules.sh). Use this instead of `git submodule update --recursive`.
set -euo pipefail
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

git submodule sync --quiet
git submodule update --init --quiet
# PS2Recomp, toml11, Catch2 and the others have no nested submodules we need.
git -C third_party/parallel-gs submodule sync --quiet Granite
git -C third_party/parallel-gs submodule update --init --quiet Granite
git -C third_party/parallel-gs/Granite submodule sync --quiet third_party/volk third_party/khronos/vulkan-headers
git -C third_party/parallel-gs/Granite submodule update --init --quiet third_party/volk third_party/khronos/vulkan-headers
echo "submodules ready"
