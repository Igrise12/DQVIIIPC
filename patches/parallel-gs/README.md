# patches/parallel-gs/

Our changes to [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) (submodule `third_party/parallel-gs`,
pinned at `cc6184a`). Upstream accepts no outside contributions (ADR-0004), so any change we need lives here as a
numbered `git format-patch` file, LGPL-3.0-or-later like the files it modifies, and is listed in the table below.

**The queue is empty.** The library `dq8-gs-parallel` builds from the unmodified submodule
(`cmake/ParallelGs.cmake`, `src/runtime/gs/parallel/`): nothing in paraLLEl-GS or Granite had to change for the
wrapper, and the C++20 deprecation of `std::is_pod` in `gs_interface.hpp` is silenced in our wrapper's translation
unit instead of patched.

| Patch | Changes | Why |
|---|---|---|
| *(none)* | | |

Spike S5's local replayer patch (`--dump-png`, `--raw`, `--timestamps`, `--backbuffer-promotion` for
`parallel-gs-replayer`, kept under `work/parallel-gs/patches/`) is not in this queue: `tools/gs/dq8-gs-replay` replays
the same dumps through our own wrapper and writes PNGs without patching upstream.

Applying a patch, when there is one: `git -C third_party/parallel-gs am ../../patches/parallel-gs/NNNN-*.patch`
after `scripts/init_submodules.sh`, then reconfigure.
