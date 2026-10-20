# patches/ps2recomp/

The patch queue for the pinned recompiler (ADR-0003). `third_party/PS2Recomp` stays at the upstream commit; every
change we need goes here as a patch, and `cmake/Ps2Recomp.cmake` builds `ps2_recomp` from a patched copy.

**The queue is empty.** Spike S1 built `eb8688b` unpatched with GCC and Clang
(`Docs/spikes/S1-ps2recomp-build.md`), so M2 uses the pinned commit as is.

## How it is applied

With `-DDQ8_BUILD_RECOMP=ON`, the download step of the `dq8_ps2recomp` external project
(`cmake/Ps2RecompPrepare.cmake`):

1. deletes `<build>/ps2recomp/source` and `<build>/ps2recomp/build`;
2. copies `third_party/PS2Recomp` to `<build>/ps2recomp/source`, without its `.git`;
3. applies every `*.patch` in this directory, in byte order of the file names, with `git apply` run in the copy;
   the first patch that does not apply fails the build and names itself.

The step re-runs (and `ps2_recomp` is rebuilt from scratch) when a patch is added, removed or edited, when the
prepare script changes, and when the submodule moves to another commit. The submodule checkout itself is never
modified.

## Writing a patch

- One concern per patch, named `NNNN-short-title.patch` (`0001-...`); the number fixes the order.
- Make it in a scratch clone of `third_party/PS2Recomp` at the pinned commit and export it with
  `git format-patch -1 --zero-commit --no-signature`, so that paths are relative to the PS2Recomp root
  (`a/ps2xRecomp/...`).
<!-- REUSE-IgnoreStart -->
- Start the commit message with two lines, `SPDX-License-Identifier: GPL-3.0-only` and
  `SPDX-FileCopyrightText: 2026 DQ8-Native contributors`, so `reuse lint` accepts the file; `git apply` ignores
  everything before the first diff. Then say why the patch exists and whether it has been offered upstream.
<!-- REUSE-IgnoreEnd -->
- Never put game-derived bytes, addresses from the disc or generated code in a patch
  (`scripts/check_no_game_data.py` reads this directory too).
- When the submodule is bumped, rebase the queue on the new commit and drop the patches upstream has taken.

## Candidates (none written yet)

From `Docs/milestones/M2-first-recompile.md` and S1:

- The emitter's inline `throw std::runtime_error(...)` for unhandled instructions, to remove the C++ exceptions
  that cross the module boundary (M3, architecture.md §13.3).
- Per-file chunking of very large functions and of `register_functions.cpp`, which only an emitter change can do
  (tech-stack.md §6.2).
- `ps2xRuntime/cmake/ReleaseMode.cmake` forces `/arch:AVX2` on MSVC Release builds, so the Windows `ps2_recomp` we
  ship would need an AVX2 CPU.
