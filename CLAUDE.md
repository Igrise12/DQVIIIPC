# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DQ8-Native: an open-source native PC port of *Dragon Quest VIII* (PS2, NTSC-U, boot file `SLUS_212.07`) by
static recompilation of the game's EE code to C++, run on a purpose-built runtime. Not an emulator. GPL-3.0-or-later.

**Current state: documentation only.** There is no source code, no build system and no CI yet. The plan of record
is the three documents in `Docs/`; read them before proposing or changing design:

- `Docs/project-brief.md`: goals, scope, facts observed from the disc (§6), feature priorities, milestones and
  spikes (§11), risks, open decisions (§14).
- `Docs/architecture.md`: setup pipeline, guest execution model, machine services, renderer, launcher, threading,
  layout rules (§13), legal safeguards (§14), extension points (§17), decision log (§18).
- `Docs/tech-stack.md`: languages, build system, dependencies and their licenses, tooling, dev-environment snapshot (§12).

Only D1 (name) and D2 (license, ADR-0009) in brief §14 are decided. D3 to D8 and ADR-0001 to 0008 and 0010 are
**proposals**; do not present them as settled. Record a newly accepted decision as `Docs/adr/NNNN-title.md`, flip
its row in architecture.md §18 and brief §14, and use absolute dates.

## Commands

No build, lint or test commands exist yet. When the first `CMakeLists.txt`, test or CI workflow lands, add the
commands here (tech-stack.md §3, §8 and §9 describe what they should be: CMake + Ninja with presets, Catch2 under
CTest with `unit` and `local` labels, clang-format/clang-tidy, a no-game-data check).

Useful today:

```bash
git check-ignore -v Game    # must print a .gitignore match; the disc image is never tracked
```

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
- Every first-party source file gets `SPDX-License-Identifier: GPL-3.0-or-later`. New dependencies must be
  GPL-3.0-compatible (tech-stack §10) and recorded in `THIRD_PARTY_NOTICES.md` in the same commit.
- Namespaces per layer: `dq8::core`, `dq8::rt`, `dq8::gs`, `dq8::features`.
- Cross-references between docs use `file.md §N` form; when you renumber a section, grep the other docs for stale refs.
- Figures in the docs that came from a single observation are marked *inferred* or "to be confirmed"; keep that
  honesty when adding facts, and say where a number came from.

## Next work (brief §11, M0 spikes)

S1 build PS2Recomp with GCC/Clang here · S2 load the ELF in Ghidra and export functions · S3 find the overlay
loader · S4 `DATA.DAT` index · S5 paraLLEl-GS standalone · S6 `MVI` format · S7 frame rate / GS usage in PCSX2.
Do not start implementation work the user has not asked for.
