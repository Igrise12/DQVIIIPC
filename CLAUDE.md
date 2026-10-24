# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DQ8-Native: an open-source native PC port of *Dragon Quest VIII* (PS2, NTSC-U, boot file `SLUS_212.07`) by
static recompilation of the game's EE code to C++, run on a purpose-built runtime. Not an emulator. GPL-3.0-or-later.

**Current state: M1 (disc intake), M2 (first recompile) and M3 (boots, done 2026-10-21) complete: the disc boots to
the title menu headless or in an SDL3 window, with paraLLEl-GS frames at PCSX2 parity. M4 (playable core) is brought up
except town, shop and battle (the owner's playtest): the field draws in 3D, the menu opens, saves are written to and
reloaded from a host-folder card, the movies play, and the game's own sound modules run on an IOP interpreter with a
register-level SPU2. M5 (launcher MVP): sessions A–E are merged (2026-10-23) and CI is green on all nine jobs at
`5744d0b` (CI's six, the AppImage, the Windows cross build under Wine); integration I1–I4 is next.**
- `src/core` (`libdq8core`): ISO 9660 reader, SHA-1/CRC-32, ELF32 reader and writer, `MWo3` parser, region profiles
  (`data/regions/SLUS_21207.toml`: hashes, sizes, addresses and the S3 loader facts, never bytes), `verify_image()` /
  `extract()`, a portable process runner, the recompile pipeline (`core/recomp/`: published and contributor function
  maps, per-unit recompiler configs, `ps2_recomp` driver, per-unit module builds), and M5's settings store
  (`config/settings/` schema, layered files), setup (`setup::run()`: stages, events, cancel and resume), toolchain
  detection and the consented llvm-mingw download, build registry, diagnostics report and input profiles.
  `dq8ctl verify|extract|info|recomp|make-unit-elf|setup|settings|run|diagnose`.
- `include/dq8/abi/`: the game-module ABI (`game_module.h`) and the include root generated code compiles against
  (`ps2recomp/`: `R5900Context`, the `PS2Runtime` adapter, the macros, the stub/syscall forwarders).
  `cmake/module/` builds one shared library per code unit (`dq8_unit_<name>`, one export).
- `src/runtime`: the machine (virtual clock, events, MMIO bus, interrupts), guest threads on fibers and the HLE
  kernel, DMAC/VIF/GIF/VU1, the GS pipeline with null, trace and paraLLEl-GS backends, SIF, the native IOP servers and
  the IOP interpreter, the SPU2, guest memory, ELF loader, dispatch tables, overrides and the overlay manager.
  `src/game`: the profile-driven game layer (residency hook, movie skip, title and 16:9 watchers, `--until`).
  `src/host`: `dq8-game`, headless or SDL3 window (window modes, aspect auto/4:3/16:9, internal resolution 1x–4x,
  bindings), `--config/--set` settings and the `--status` channel. `src/launcher`: `dq8-launcher` (Dear ImGui over
  SDL3: setup wizard, Play, Settings, Controllers, Diagnostics, About).
- Published function maps in `data/maps/SLUS_21207/` (addresses, sizes, table shapes; ADR-0017): a user's setup needs
  no Ghidra. `cmake --install` gives a relocatable `bin/ lib/ share/dq8/` tree; `packaging/linux/` builds the
  AppImage, `packaging/windows/` the zip (cross-built with llvm-mingw, ADR-0018).
- `dq8-game --until title` boots the disc to the title menu in 1,200 vblanks, deterministically (the M3 input under
  `--movies blank`; the default `--movies skip` also skips the title's flyover timeline); `--window` plays it live
  (see `Docs/milestones/M3-boots.md` §Results). README.md has the build, install and launcher steps.
- Tests: 726 unit tests (CI on GCC, Clang, MSVC, ASan; the Windows cross build runs 718 of them under Wine), local
  disc tests, a synthetic machine run pinned to golden hashes, and a synthetic recompile end-to-end suite (label
  `e2e`, CI job on GCC 13). A failed CI test step turns each failed test into an annotation
  (`scripts/ci_test_annotations.py`), readable without signing in to GitHub.

The plan of record is the three documents in `Docs/`; read them before
proposing or changing design:

- `Docs/project-brief.md`: goals, scope, facts observed from the disc (§6), feature priorities, milestones and
  spikes (§11), risks, open decisions (§14).
- `Docs/architecture.md`: setup pipeline, guest execution model, machine services, renderer, launcher, threading,
  layout rules (§13), legal safeguards (§14), extension points (§17), decision log (§18).
- `Docs/tech-stack.md`: languages, build system, dependencies and their licenses, tooling, dev-environment snapshot (§12).
- `Docs/spikes/`: one brief per M0 spike (S0–S7); a spike session executes its brief and appends `## Findings`.
  `Docs/spikes/README.md` has the rules and the findings template.
- `Docs/milestones/`: one shared brief and results record per implementation milestone (M2 onward).
- `Docs/formats/`: layouts of the game's file formats (no game-derived values).

Decided in brief §14: D1 (name), D2 (license, ADR-0009), D3 (local game-module build, ADR-0008), D4 (PS2Recomp pinned
plus patch queue, ADR-0003) and D5 (paraLLEl-GS first, ADR-0004); with M3 (2026-10-21) ADR-0005 (SDL3), 0007 (machine
thread and fibers), 0011 (native IOP servers first) and 0012 (overlay residency through the loader hook); with M4
(2026-10-21) ADR-0014 (sound modules on an IOP interpreter), 0015 (movies through an sceMpeg HLE with libavcodec) and
0016 (host-folder memory card); with M5 (2026-10-22) ADR-0006 (schema-driven settings), 0010 (Dear ImGui launcher,
D6), 0017 (published address-only function maps, D9) and 0018 (llvm-mingw for Windows host and modules). D7, D8 and
ADR-0001, 0002 and 0013 (guarded guest-state writes, implemented behind `--movies skip`) are **proposals**; do not
present them as settled. Record a newly accepted decision as `Docs/adr/NNNN-title.md`, flip
its row in architecture.md §18 and brief §14, and use absolute dates.

## Commands

```bash
cmake --preset dev-clang && cmake --build --preset dev-clang --parallel   # presets: dev-gcc, dev-clang, release, asan, ci-*
cmake --preset dev-clang -DCMAKE_CXX_COMPILER_LAUNCHER=ccache   # optional: cache object files between rebuilds
ctest --preset unit-dev-clang                      # unit tests (label "unit", no game data); CI runs these
ctest --preset unit-dev-clang -R '<regex>'         # one test by name
./build/dev-clang/tests/unit/dq8_unit_tests '<test name>'   # run a single Catch2 test directly
DQ8_TEST_ISO="$DQ8_ISO" ctest --preset local-dev-clang   # label "local": tests against your disc; never in CI
./build/dev-clang/src/cli/dq8ctl verify "$DQ8_ISO" [--json]   # region gate: boot-file hash vs data/regions/*.toml
./build/dev-clang/src/cli/dq8ctl extract "$DQ8_ISO" [--out DIR] [--emit-profile]   # ELF, overlays, IOP modules -> baserom/<id>/ (idempotent)
./build/dev-clang/src/cli/dq8ctl info <iso|elf>    # volume listing or ELF headers
cmake --preset ci-gcc -B build/e2e -DDQ8_BUILD_RECOMP=ON -DDQ8_BUILD_E2E=ON && cmake --build build/e2e -j8   # also builds ps2_recomp
ctest --test-dir build/e2e -L e2e                  # synthetic recompile end-to-end (no game data)
tools/ghidra/export_maps.sh                        # seven function maps -> work/recomp/maps (needs the Ghidra projects)
build/e2e/src/cli/dq8ctl recomp all "$DQ8_ISO" --work work/recomp --maps work/recomp/maps \
    --ps2-recomp build/e2e/ps2recomp/build/ps2xRecomp/ps2_recomp --module-template cmake/module --abi-include-dir include
build/e2e/src/host/dq8-game --iso "$DQ8_ISO" --module-dir work/recomp/modules/SLUS_21207 --stop-at-first-syscall [--json]
python3 -I scripts/check_no_game_data.py --tree [--deep]   # the legal gate; --staged is the pre-commit hook
pre-commit install                                 # once per clone: clang-format + the checker on every commit
git ls-files -z -- 'src/*' 'include/*' 'tests/*' | grep -zE '\.(cpp|h)$' | xargs -0 clang-format --dry-run -Werror
```

Every first-party target goes through `dq8_set_warnings()` and `dq8_enable_sanitizers()` (`cmake/`). New
dependencies are git submodules under `third_party/` plus a row in `THIRD_PARTY_NOTICES.md`.

Contributor tooling:

```bash
git check-ignore -v Game    # must print a .gitignore match; the disc image is never tracked
cp tools/local.env.example tools/local.env    # then fill in your paths (git-ignored)
set -a; . tools/local.env; set +a; 7z x -y -obaserom/raw "$DQ8_ISO" SLUS_212.07 SYSTEM.CNF DATA.HD6 DATA2.HD6 BIN MODULES
GHIDRA_INSTALL_DIR="$DQ8_GHIDRA_INSTALL_DIR" rea doctor --provider ghidra --format json    # REA (Ghidra MCP/CLI) health
```

Scratch locations, all git-ignored: `baserom/` (extracted disc files), `work/` (clones, Ghidra projects, dumps,
REA snapshots), `generated/` (recompiler output). Ghidra and PCSX2 are not installed system-wide; `tools/local.env`
points at the copies in the sibling `MGS2_Undub` project.

`origin` is `git@github.com:Igrise12/DQVIIIPC.git` (SSH; there are no HTTPS credentials on this machine).

## Hard rules

**No game-derived content, ever** (brief §9, architecture §14). Not in commits, not in docs, not in test fixtures:
no bytes from the ELF or overlays, no extracted files, no generated C++, no asset or text content, no BIOS.
Allowed: addresses, sizes, names we authored, expected hashes, and shape-only tables (labels and sizes, no values).
`Game/`, `baserom/`, `generated/`, `*.iso`, `*.elf`, `*.irx`, `SLUS_*`, `DATA*.DAT` are git-ignored; keep it that way.
Anything you extract from the disc for inspection goes in the scratchpad, not the repo, and is deleted afterwards.

**The user supplies the disc.** The ISO at `Game/*.iso` is for local analysis only.

## Architecture in one screen

Details and rationale are in `Docs/architecture.md`; these are the constraints that shape every component.

- **Host vs game module (§2).** The redistributable *host* (launcher, CLI, runtime, renderers, features) is ours.
  The *game module* (recompiled game code) is built on the user's machine from their disc and loaded through a
  small versioned **C ABI** in `include/dq8/abi/`. Nothing crosses that boundary except plain C.
- **Overlays (§4.3).** The boot ELF is stripped (0 symbols), and six code overlays (`BIN/*.BIN`, `MWo3` container)
  share one address window at `0x00461C80`. Each is a separate *code unit* with its own function map; an
  `OverlayManager` repoints the dispatch table for that window. Nothing in `src/runtime/` may hardcode the window
  address or overlay names; they come from region profiles in `data/regions/`.
- **Native, not emulated (§2.3, §5).** Kernel and IOP services are HLE, DMA/VIF/GIF are modelled at packet level,
  VU1 is interpreter-then-AOT, the GS sits behind `IGsBackend` (paraLLEl-GS first, trace backend for tests,
  native renderer later). No BIOS, no cycle timing.
- **Determinism (§12).** One machine thread runs EE, IOP and DMA in virtual time with fibers for guest threads;
  render, audio and I/O are separate threads. Wall-clock never reaches the guest. Interrupts are delivered only at
  safe points.
- **Arithmetic fidelity (§4.4).** EE FPU is not IEEE-754 (round toward zero, no NaN/Inf). Use the dedicated
  helpers, never raw host float ops, in anything that reproduces guest behaviour.
- **Data-driven (§9, §10).** Settings come from a schema; the launcher renders UI from it. Features declare a
  manifest and settings. Patches are declarative data keyed by (region profile, code unit, address) and carry only
  our replacement values plus a digest of the target, never original bytes.

## Layering (architecture §13.2)

```
core  <-  runtime  <-  game, features  <-  host
launcher, cli  ->  core only
```

- `src/core` knows nothing about the PS2 or any UI.
- `src/runtime` knows nothing about Dragon Quest. Game- and region-specific code goes in `src/game/` or data files.
- `src/launcher` and `src/cli` never link the runtime; they spawn `dq8-game` and talk over JSON-lines events.
- `src/features` use public interfaces in `include/dq8/`, not runtime internals.
- Each top-level source directory has a short `README.md` stating its purpose and rules; keep them current.

## Conventions

- C++20 for everything players run; Python and Java (Ghidra scripts) are contributor tools only and never on the
  end-user setup path. Scripts that read game files run with `python3 -I`.
- `.clang-format` (LLVM base, 4 spaces, 110 columns, `PointerAlignment: Left`) and `.editorconfig` are authoritative.
- Every first-party source file gets an SPDX header (`SPDX-License-Identifier` set to `GPL-3.0-or-later`). New dependencies must be
  GPL-3.0-compatible (tech-stack §10) and recorded in `THIRD_PARTY_NOTICES.md` in the same commit.
- Namespaces per layer: `dq8::core`, `dq8::rt`, `dq8::gs`, `dq8::features`.
- Cross-references between docs use `file.md §N` form; when you renumber a section, grep the other docs for stale refs.
- Figures in the docs that came from a single observation are marked *inferred* or "to be confirmed"; keep that
  honesty when adding facts, and say where a number came from.

## Next work

All M0 spikes (S0–S7) are done; their findings live in `Docs/spikes/` and are folded into the three docs and
`Docs/formats/`. **M3 (boots) is done** (2026-10-21, `Docs/milestones/M3-boots.md`): day 0, sessions A–E, and the
bring-up on the real disc; the headless boot reaches the title menu deterministically, paraLLEl-GS matches PCSX2's
software renderer (settings 0.9998, title logo 0.9989 SSIM), and `--window` runs at real time with keyboard and
gamepad (`tools/bringup/window_smoke.py`). CI is green on all six jobs at `38fdf4d`.

**M4 (playable core)**, planned with the project owner on 2026-10-21 (`Docs/milestones/M4-playable.md`; ADRs
0014–0016): day 0, sessions A–D and the VU1 interpreter speed-up are merged; bring-up B1 (field 3D), B2 (menu), B5/B6
(save and reload), B7 (movies) and B8 (sound) are done; B3/B4 (town, shop, battle) are the owner's playtest. Open: the
menu's separator dots, two bright glows, WAV and frame parity tooling, and speed (about 13 vblanks/s in the field with
the release preset; M6). The movie geometry item is resolved by M5's `--aspect auto` following the game's 16:9 option.

**M5 (launcher MVP)**, planned with the project owner on 2026-10-22 (`Docs/milestones/M5-launcher.md`; ADRs 0006,
0010, 0017, 0018): Linux and Windows; a new user goes from the ISO to playing without a terminal.
- merged 2026-10-23, in order A → C → D → B → E, each rebased in its worktree with its integration fixes as branch
  commits: A (settings, setup, toolchain, registry, dq8ctl), C (display, input, audio, 16:9), D (published maps,
  install layout, bundled LGPL FFmpeg, AppImage, the checker's `--release` scan), B (launcher), E (Windows cross build,
  toolchain download, zip). Fixes found on the way: module builds reconfigure with `cmake --fresh` when the template
  moved (AppImage mounts), test scripts start Windows programs through a Wine wrapper (`dq8_test_program`; GitHub's
  runner has no working binfmt_misc), data lookups search 8 directories up (MSVC's `Release/`), MSVC entry and test
  fixes. On the disc: a fresh `dq8ctl setup` from the published maps passes every stage in about 10 minutes and its
  modules boot to the M4 golden title; 2x internal resolution and 16:9 auto checked in the window.
- next: integration I1 (settings round trip through the launcher), I2 (the launcher's wizard and Play on the disc
  from a clean profile; not yet tried), I3 (Windows: Wine smoke here, then the owner on real Windows), I4 (legal scan
  of the AppImage and zip, notices for every bundled binary).
- waiting on the owner: `claude/serene-meitner-fc10fd` (SDL submodule from an unreleased 3.5 snapshot to
  release-3.4.18, as ADR-0005 decided) and `claude/gallant-chatterjee-39e89a` (`tools/bringup/window_smoke.py` timed
  from the run's events); neither is merged.

M2's, M3's and M4's records are `Docs/milestones/M2-first-recompile.md`, `M3-boots.md` and `M4-playable.md`.
Do not start implementation work the user has not asked for.
