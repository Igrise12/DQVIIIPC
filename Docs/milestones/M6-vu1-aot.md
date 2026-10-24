# M6: VU1 ahead-of-time compilation

| | |
|---|---|
| **Status** | Planned 2026-10-24 with the project owner. Nothing is implemented; phase P0 (measure) comes next, then the owner's checkpoint before day 0 |
| **Scope** | Stage 2 of architecture.md §5.4, the "full speed" half of M6 (brief §11). The playthrough half of M6 gets its own brief |
| **Exit** | On the dev box (Ryzen 5 5600, release preset) the field and the opening scene of the New Game probe run at full speed or better, and the compiled VU1 programs leave every deterministic output byte-identical with the interpreter |
| **Owner choices (2026-10-24)** | Exact cycle counts as well as values, memory and packets · the census and Optimize, plus a published address-only program list if P0 finds the programs at fixed places on the disc (its own ADR) · Optimize as a button with a one-time prompt · of the smaller wins only the SHA-NI SHA-1, merged first (no `--gs-hash off`, no tree-wide `-ffp-contract=off`, no `perf` install: P0 samples with gdb) |
| **Proposed, for the owner** | ADR-0019 (VU1 programs compiled ahead of time into a locally built companion library, matched by content, with the interpreter as reference and fallback) |

Sessions read this file. Contract changes go through the coordinator. Every rule in CLAUDE.md applies. The most
important is that **no game-derived content** goes into commits. VU1 programs are uploaded from game data, so the
census, the generated C++, the compiled library and any recorded entry state stay local, under `work/` or the user
data directory. Docs carry counts, sizes, micro addresses, guest addresses and hashes only.

## What exploration established

**Speed today** (measured 2026-10-24, release preset (RelWithDebInfo), RTX 4060, Ryzen 5 5600). The New Game probe
is `work/bringup/probes/newgame.dq8in` with `--movies blank --vblanks 2300`. The field and the opening scene run at
about 12 vblanks/s (speed 0.20), and the heavier stretch near vblank 2300 at about 9.5 vblanks/s (0.16). The probe
takes 30.7 s with `--gs parallel` and 29.8 s with `--gs null`, so GPU rendering costs nothing here. The machine
thread is pinned at one core. The boot movies decode at 6–8x real time, so movies are not the problem.

**Where the time goes, and what is not yet known.** M4's stack samples (M4-playable.md, bring-up log, "Where the
field's time goes") put about 94 % of the machine thread in the VU1 interpreter. Two caveats change the arithmetic:

- Those samples were taken *before* M4's interpreter speed-up. The probe then took 74 s, and its boot to vblank
  1950 (4.6 s, with no VU1 work) was 6 % of it.
- *After* the speed-up (31 s), the GS stream's SHA-1 alone was about 8 % of the probe. So the work that is not VU1 is
  now a larger share. If it stayed at about 4 s while VU1 shrank, it is roughly 15 % of the field's time
  (*inferred*, not measured).

By Amdahl's law the factor VU1 needs depends on that share:

| Non-VU1 share of the field | VU1 speed-up for real time at speed 0.20 | at 0.16 |
|---|---|---|
| 6 % (M4's samples) | about 7x | about 10x |
| 15 % (*inferred* after the speed-up) | about 17x | unreachable unless the non-VU1 work also shrinks |

The same session's notes say the interpreter's profile is now flat: per-lane loops and the x87 sums dominate, and
further gains need a structural change. The first thing P0 does is profile the current build again. The GS hash item
below matters for the same reason.

**How VU1 runs in the machine** (code as of `f1c383f`).
- VIF1's MSCAL, MSCALF and MSCNT call `VuUnits::start` and `resume` (`dma/vif.cpp`, `Vif::start_micro`).
  `VuUnits::run` runs the program to its end before the VIF reads its next code. A program that does not end within
  `max_program_cycles` (16 Mi cycles) stops the machine with a diagnostic.
- **VU1 cycles never reach the machine clock.** They appear only in the `--json` report's `machine.vu` section
  (programs, cycles, XGKICKs per unit). VU0 programs started by VCALLMS do charge the EE clock through the VU0
  bridge, but they are short (about 25 cycles each) and out of scope here.
- **Every program starts with empty pipelines.** MSCAL resets the scheduler (`execute` calls `resetScheduler`).
  Every program ends in `flushPipelines`, which also waits for a PATH1 transfer to finish. MSCNT therefore resumes
  from a drained state. Program boundaries are clean seams: the full entry state is `VuState`, VU1 data memory and
  micro memory.
- **XGKICK** (`vu_core.cpp`) copies one qword every two cycles from VU1 data memory, and the issue cycle counts as the
  first. A store that commits at cycle t is visible to the qword copied at t. The packet goes to the GIF's PATH1 only
  at its EOP tag. While a transfer is active, a second XGKICK waits. The transfer's length comes from the GIF tags'
  NLOOP and NREG, so it depends on data.
- **Observable timing inside a program.** VF, VI and ACC reads stall until their writes commit. A read can still see
  an old value in several cases:
  - Q and P before FDIV or EFU completes, without WAITQ or WAITP;
  - MAC, status and clip flags through their pipeline;
  - the conditional-branch VI rule;
  - a load issued before a pending store commits;
  - the PATH1 copy against store commits.

  So values depend on timing, and the cycle count is needed for values even where nobody reads the count.
- **The decode cache** follows `IVuHost::code_generation()`. The counter changes on every MPG (`VuUnits::write_micro`)
  and every EE store into micro memory.

**What a "program" is.** VU1 micro memory is 16 KiB. The field uploads the same 88-doubleword program at micro
address 0 every frame: 1,006 MPGs by vblank 2600 (M4 bring-up, "The field draws no 3D"). Its programs also jump
through VI registers into routines uploaded separately. The JALR target 0x960 is one (M4, "VU1 kicks only empty GIF
tags"). Programs are started by MSCAL and continued by MSCNT at the pc where the last one ended: 123,094 MSCALs and
301,100 MSCNTs by vblank 2600. So the unit of compilation is **an entry pc plus the code reachable from it**, not an
MPG payload. Its identity is the content of that code, wherever the bytes came from.

**Volume** (M4 speed-up row, the probe at vblank 2300 before the sound modules ran interpreted): 185,685 VU1
programs, 383,227,873 cycles and 879,802 XGKICKs. That is about 2,060 cycles and 4.7 kicks per program. With the sound
modules interpreted (B8) the count at vblank 2300 is 159,743. VU1's work starts in the field. At about 27 s for the
field stretch, the interpreter manages roughly 15 M VU cycles per host second (*inferred*). At about 4.4 GHz that is
about 300 host cycles per VU cycle. If most of VU1's work runs from about vblank 1950, as the boot's 4.6 s to that
point suggests, the game asks for about 66 M VU cycles per guest second (383 M over 350 vblanks; *inferred*), and more in
the heavy stretch.

**Tooling on hand.**
- `perf` and `valgrind` are still not installed. M4 sampled with gdb. `ccache` is installed.
- The M4 differential fuzzer was **not committed**: no fuzz code is in the repository. Its source survives outside
  the repository, in the notes of the M4 speed-up session (`vu1-speedup-difffuzz.cpp`, 407 lines; old interpreter
  against new, slice by slice). Its generator makes random programs with branches, XGKICK packets, D/T/E bits, reserved
  opcodes, sliced budgets and EE stores between slices, and it has an FMAC mode with boundary operands. Its last run
  covered 32,000 programs (1.27 M slices, 1.1 M packets) and 4 M boundary FMACs with no difference. P0 copies it to
  `work/vu1/`. Session B starts from it, compares compiled against interpreted runs, and commits it.
- No build file sets `-ffp-contract`, `-march` or fast-math flags. On baseline x86-64 no FMA is emitted, so the
  interpreter is safe today. A CPU-specific or ARM64 build could contract `a*b+c` and change results.
- `core::Sha1` (`src/core/hash/hash.cpp`) is a portable implementation.

**Prior art.**
- PCSX2's microVU (GPL-3.0-or-later) is a recompiler. As we understand it (to be confirmed when read), it keys
  compiled blocks by the pipeline state at block entry, checks a program's identity by the micro-memory ranges it
  covers, and computes flags only where something reads them. We would read it as a reference only, as M4 read
  PCSX2's SPU2, and adapt no code.
- ico-recomp and god-of-war-recomp also compile VU1 programs ahead of time (brief §15). How exact their timing is has
  not been checked.

## Design

### The contract: what a compiled program preserves

For any entry state, a compiled run and an interpreter run must end with the same results:

- the same `VuState`: VF, VI, ACC, Q, P, I, R, MAC, clip, status with sticky bits, and the pc;
- the same VU1 data memory;
- the same PATH1 packets, byte for byte and in the same order;
- the same cycle count;
- the same stop, when one occurs: a reserved instruction, a malformed XGKICK packet or the cycle budget, stopping at
  the same pc and cycle.

The cycle count is kept although nothing reads VU1 time today. The static timing model is needed for the values
anyway (see above), so keeping the count costs nothing extra. It also keeps the door open to charging VU1 time to
the clock later.

**The interpreter stays the reference.** Anything the compiled code does not model exactly is a **bail-out**: at an
instruction-pair boundary the compiled code writes the full pipeline state into a resume record, and the interpreter
finishes the program. Correctness never depends on the compiled set. A missing, stale or partial library gives the
same outputs, only slower.

### 1. The program census

The census is the runtime's record of every VU1 program it meets. It is written by `dq8-game` and read by the
compiler. It is local and never committed.

- **Key.** (unit, entry pc, closure). The closure is the set of instruction pairs reachable from the entry. It covers
  static branches, the fall-through up to an E bit plus its delay slot, and the JR/JALR targets seen so far. It is
  recorded as micro-address ranges, with the SHA-1 of their bytes and their addresses. The field's per-frame
  re-upload changes the generation but not the content, so it finds the same entry.
- **When it records.** It records on the first MSCAL or MSCNT at a pc after a generation change, and only when
  neither the compiled set nor the census knows the closure. The runtime builds the closure with the interpreter's own
  decoder. It hashes and copies the closure's bytes once per new program. Runs, cycles, XGKICKs, observed indirect
  targets and bail-out counts are counters. While the census is collecting coverage, the interpreter sets one bit per
  executed pair through a coverage hook, and only for programs not yet in the census. Otherwise the hook is a null
  pointer, so the hot loop pays for one predictable branch.
- **Where.** `<user data dir>/vu1/<profile>/census.toml` holds the index. `code/<sha1>.bin` holds the closure bytes.
  Contributors pass `--vu1-census work/vu1/census`. The I/O thread writes it when new programs appear (debounced) and
  at exit, by atomic replace. It never touches guest state, events or the `--json` report. Headless runs record
  nothing unless `--vu1-census DIR` is given, so tests and goldens are unaffected. Window runs record into the user
  data directory by default, through a new schema row `game.vu1_census` (default on, advanced).
- **Samples for local tests** (contributors only, `--vu1-census-samples N`). These are up to N entry states per program
  (`VuState` plus the 16 KiB data memory), for the census replay test below. They hold game data, so they are local
  only.
- `Docs/formats/vu1-census.md` (day 0) documents the index's keys. The checker refuses census paths and
  `census.toml`.

### 2. The compiler (`tools/vu1/`, `dq8-vu1c`)

`dq8-vu1c` is C++20. It is a contributor tool and also ships with the host, like `ps2_recomp`, because "Optimize"
runs it on the user's machine. It reads the census and writes one C++ file per program, plus the library's descriptor.
The same census always gives byte-identical C++. The generator string carries the compiler version and the ISA digest
(below).

- **One decoder, one timing model, one arithmetic.** On day 0, decode and usage (`decodeInstructionPair`, the
  latency tables) move into `src/runtime/vu/isa/`, which both the interpreter and the compiler link. FMAC arithmetic,
  normalization and flag classification move into a header, `include/dq8/abi/vu1/vu_arith.h`, which the interpreter
  and the generated code both include. The move changes no behaviour. A digest of the ISA tables and the arithmetic
  version goes into every library, and the host refuses a library built against another digest.
- **The interpreter is the timing oracle.** The compiler links the interpreter and runs each basic block, from a given
  entry timing state, with placeholder values. It records when each pair issues and when each pipeline entry commits.
  This is exact because inside a block timing does not depend on values; the one exception is PATH1, handled below.
  Code generation follows the recorded schedule, so the static model cannot drift from the interpreter's rules.
- **Block versions.** The entry timing state is the set of in-flight items with their remaining cycles: VF lanes, VI,
  ACC, flag entries, FDIV, EFU, stores and the VI branch backup. A worklist explores the (block, timing state) pairs
  reachable from each entry, starting from the drained state. Loops converge because the latencies are bounded (the
  longest, EFU, is 54 cycles). A block that exceeds a cap (8 versions, to be tuned in P0) gets a bail-out at its entry
  for the extra states.
- **Generated code shape.** There is one function per (entry pc, closure). Block versions are labels. VF, VI, ACC, Q,
  P, I and R live in locals (SSE for VF), and pending results are temporaries assigned at their commit point. Memory
  writes happen in commit order. The cycle counter adds a constant per block version. MAC, status and clip are
  computed only where something reads them or where they are live at the program's end. Sticky status bits use a
  cheap exponent test, with the shared exact classification (`vu_arith.h`) as the slow path. Values are written back
  to `VuState` at the end or at a bail-out.
- **PATH1 is lazy.** The compiled code keeps a small cursor for the active transfer: source, bytes copied, current tag
  end and EOP. It advances the cursor, copying and parsing tags with the shared code, only when something could
  observe it:
  - before a store commits while a transfer is active;
  - at the next XGKICK;
  - at the end of the program.

  This reproduces the interpreter's packets exactly. At a second XGKICK whose transfer is still running, the stall
  length depends on data. If every other pipeline is drained at that point, the state after the stall is drained
  whatever the length, so one version follows. Otherwise the program bails out. P0 counts how often each case occurs.
  The program's last cycle is the later of the static end and the end of the transfer.
- **Everything else is a bail-out:**
  - JR or JALR to a target not in the closure (a switch covers the known targets);
  - a timing state with no version;
  - a malformed GIF tag ahead (the interpreter then reports it at the same cycle);
  - a reserved instruction (the bail-out happens before it);
  - the cycle budget, checked at back edges;
  - D or T bits when FBRST enables them. These are part of the entry key; a program that has them gets both versions,
    if P0 finds any.

### 3. Linking and matching

- **A companion library, not the game modules.** The compiled programs form one shared library per profile,
  `dq8_vu1_<profile>`. It is built locally from the module template machinery with the user's toolchain (ADR-0008; on
  Windows the pinned llvm-mingw, ADR-0018). It is independent of the EE game modules, so "Optimize" rebuilds it without
  touching them, and an EE rebuild does not invalidate it.
- **The ABI** (`include/dq8/abi/vu1_program.h`, plain C, versioned independently of the game-module ABI) contains:
  - one export, `dq8_vu1_library_describe`, returning a descriptor with the magic, ABI version, profile id, generator,
    ISA digest and the program table;
  - per program: the entry pc, the closure ranges and their bytes (local, never distributed), and a function pointer;
  - a run context: pointers to a C mirror of `VuState`, the data memory, the cycle and budget, TOP and ITOP, the
    XGKICK callback and the resume record;
  - return codes: ended, bailed out, stopped.

  Nothing in the compiled code throws.
- **Build flags.** `-O2 -ffp-contract=off`, never fast-math. The ISA is baseline x86-64 unless the owner opts into a
  CPU-specific build after V2 (see "Owner decisions"). The library records its flags.
- **Matching at run time** (`VuUnits`, unit 1). For each generation the runtime caches an entry table: 2,048 slots,
  one per pair address, filled lazily. On the first MSCAL or MSCNT at a pc after a generation change, it compares the
  candidates' closure bytes for that pc with micro memory (memcmp, a few KiB at most). A match runs the compiled
  function. A miss runs the interpreter and goes to the census. Stats (`programs`, `cycles`, `xgkicks`) are updated
  exactly as the interpreter path updates them.
- **Bail-out.** The interpreter gains `export_snapshot()` and `adopt(const dq8_vu1_resume&)`. The resume record holds
  the pc, branch and halt state, pending writes, stores and flag entries with remaining cycles, FDIV and EFU, the
  PATH1 cursor with the bytes copied so far, and the VI branch backup. The interpreter then continues with `resume`.
- **Loading.** `dq8-game` loads `<module dir>/dq8_vu1_<profile>` (or `--vu1-library FILE`) and checks the magic, ABI
  version, profile and ISA digest. A bad library is not loaded: the run continues on the interpreter and sends a
  `warning` status line (`vu1-library-stale`), and the launcher offers Optimize. `--vu1 interp|aot|verify` picks the
  mode; `aot` is the default when a library is present.
- **Status, not events.** Hits, misses, bail-outs by reason and new census entries go on the `--status` channel
  (`{"ev":"vu1",...}` once a second). They never go into `--json` or `--events`, which must stay identical across
  modes.

### 4. Verification

| Level | What | Where |
|---|---|---|
| Unit (CI) | A corpus of synthetic VU1 programs, made by our generator with fixed seeds, covering branches, JR/JALR, XGKICK packets that race stores, D/T/E bits, all latencies, and FMACs with operands at the overflow and underflow edges. They are compiled at build time and run against the interpreter from random entry states. State, data memory, packets and cycles must be equal. | `tests/unit`, Linux, MSVC and Wine jobs |
| Snapshot round trip (CI) | On the corpus, run the interpreter to pair k, export, adopt into a fresh interpreter and continue. The result must equal an uninterrupted run for every k. This proves bail-out at any boundary. | `tests/unit` |
| Fuzzer (e2e) | M4's differential fuzzer, rewired, generates random programs, compiles them with `dq8-vu1c`, builds the library and runs it in lockstep with the interpreter. Seeded faults must be caught: a skipped commit cycle, a product rounded to float, a reversed commit order, a short PATH1 copy, a lost sticky bit, a store committed one cycle late. | label `e2e`, the GCC 13 CI job |
| Census replay (local) | Every census program, from its recorded entry samples, compiled against the interpreter. | `work/vu1/`, label `local` |
| Lockstep on the disc (local) | `--vu1 verify` runs the interpreter for real and the compiled program on a shadow copy, then compares after every program. The first mismatch stops the run, naming the entry pc and the first differing field, and writes the entry state to `work/` as a reproducer. | bring-up |
| End to end (local) | The New Game probe with `--vu1 interp` and `--vu1 aot` must give a byte-identical `--json` report, `--events` log, `gs_hash`, `--gs-trace` file (SHA-1 compared, file deleted), RAM SHA-1 at vblank 2300 and audio hash, with `--gs null` and `--gs trace`. M3 and M4 goldens are unchanged in both modes, and Debug equals Release. | bring-up V2, acceptance |

The library also runs a **self-test on the user's machine**. `dq8-game --vu1-selftest FILE` loads it and runs each
program from synthetic entry states in lockstep with the interpreter, with a bounded number of cycles. It needs no
game state beyond the code bytes the library embeds. This catches a miscompile by the user's compiler before play. A
library that fails it is not registered.

### 5. Optimize (launcher and `dq8ctl`)

- **Core API.** `core::vu1::optimize()` uses the setup's event format, toolchain detection, job count, cancel and
  resume. Its stages are `census` (read and validate), `compile` (`dq8-vu1c`), `build`, `selftest` (`dq8-game
  --vu1-selftest`, a process: the launcher and core never link the runtime) and `register` (the registry gets the
  library path and the census digest it was built from). `dq8ctl vu1 status|optimize|clear` drives it.
- **Setup.** A new stage `vu1` runs after `build` and before `smoke`. It compiles what is known: the published list,
  if P0 allows it (the owner's choice, below), plus any census. With nothing known it is `skipped`.
- **Launcher.**
  - The Play page shows "Graphics programs: N compiled, M new since the last optimize" with an Optimize button.
  - After a session that recorded new programs, the page asks once whether to optimize now, with the estimated time.
  - Diagnostics includes the census counts, never its bytes.
- **The first-play problem.** With the census alone, a new user's first visit to the field runs at about 0.2 speed
  until they quit and optimize. The owner chose remedy (a), on condition of P0; (b) is not planned:
  - (a) **A published program list** (`data/vu1/<profile>/programs.toml`, address-only, like ADR-0017's maps). For
    each program it gives the source of its bytes on the user's disc (file and offset, or a guest address in a unit
    image), size, SHA-1, entry pcs and indirect targets. Setup then compiles the known programs before the first play.
    This works only if P0 finds that the programs come from the ELF, an overlay or a fixed place in the archives. It
    gets its own ADR and checker rules, as maps did, and session C's setup stage `vu1` reads it.
  - (b) **Hot-loading.** Because compiled and interpreted runs are identical by contract, a library can be switched in
    at any program boundary without changing any output. The launcher could optimize in the background and
    `dq8-game` pick the new library up between VIF commands. Not planned (owner, 2026-10-24).

### 6. The speed target and how it is measured

**Measurement.** Use the release preset, the same probe and the `--status` channel. Each `fps` line covers one wall
second. Headless, `fps` counts vblanks and `speed` is guest time over wall time. Day 0 adds `vblank` to the line, so
lines map to segments without summing (the launcher ignores unknown fields).

```bash
set -a; . tools/local.env; set +a
build/release/src/host/dq8-game --iso "$DQ8_ISO" --module-dir work/recomp/modules/SLUS_21207 \
    --input-script work/bringup/probes/newgame.dq8in --movies blank --vblanks 2300 --gs null \
    --json --status work/vu1/runs/<name>-status.jsonl --vu1 aot > work/vu1/runs/<name>.json
```

The run is repeated with `--gs parallel`, and live with `--window --speed 1` (the limiter's `speed` should read
1.00). Each configuration runs three times back to back on an idle host, and the median is recorded.

**Segments.**
- boot: vblanks 0–1950
- field: vblanks 1950–2200
- heavy: vblanks 2200–2300
- the opening scene: a longer run to vblank 3600, the probe's dialogue and glow

**Machine-thread breakdown.** `--status` gains a `profile` line once a second, enabled with `--profile`. It is host
time only: wall-clock shares of VU1, the GS hash, VIF/DMA/GIF, IOP/SPU2, EE code and other. It uses two
`steady_clock` reads per VU1 program, about 7 ms over the probe (*estimate*). It never reaches the guest. gdb
sampling confirms the breakdown; `perf` stays uninstalled (owner, 2026-10-24).

| Measure | Today (2026-10-24) | Target |
|---|---|---|
| Field, headless null, `speed` | 0.20 | **≥ 1.0 in every line**; stretch 1.5 |
| Heavy stretch, headless null | 0.16 | **≥ 1.0** |
| Window, parallel GS, `--speed 1` | about 0.2 | holds 1.00 (no field line below 0.98) |
| VU1 throughput (VU cycles per host second, compiled programs) | about 15 M (*inferred*) | **≥ 150 M** (10x) |
| Non-VU1 machine time per guest second in the field | about 0.3–0.7 s (*inferred*) | ≤ 0.25 s (with session D's SHA-NI hash) |
| Programs run compiled in the probe after one optimize | 0 | ≥ 99 % of VU1 cycles, bail-outs < 0.1 % of programs |

Debug numbers and the other two configurations are recorded, not gated. Slower CPUs are not gated either. The
stretch target is their headroom.

## Smaller wins found along the way

| Item | Finding | Proposal |
|---|---|---|
| GS stream SHA-1 | About 8 % of the probe, on the machine thread, also with `--window`. It is a chained SHA-1 over every GIF transfer and vsync record (`GsPipeline::gif_transfer`, `vsync`). The per-vblank `gs` events and the end report read it. | **Approved (2026-10-24), session D, merged first:** a SHA-NI path in `core::Sha1`, with runtime CPU detection and the portable code as fallback. Digests are identical, so every golden holds; a test compares it with the portable code on random input. Not approved: `--gs-hash off` for window play (window runs keep hashing). Moving the hash to the render thread is rejected: each vblank's `gs` event would wait for that thread. |
| A separate VU1 thread (PCSX2's MTVU) | It overlaps only the non-VU1 share. Determinism would need a sync point at every observation: the next VIF command that waits (FLUSH*, MSCAL), EE reads of VU1 memory, VPU_STAT, and every GIF path switch, since PATH1 packets must keep their place against PATH2 and PATH3. | Not now. Revisit after AOT, if VU1 is still more than half of the machine thread. |
| VU1 on the GPU | The programs are serial, branchy and short, and their results feed the GS in order. A GPU round trip per program would cost more than the program. | Rejected. |
| Floating-point flags | No `-ffp-contract=off` anywhere. Safe on baseline x86-64, but a CPU-specific build, ARM64 (Tier 2; clang contracts by default) or the VU1 library could change results. | Not approved tree-wide (2026-10-24); stays a finding for the ARM64 port. The VU1 library alone builds with `-ffp-contract=off`, which the contract needs. |
| Per-transfer allocation | `GsPipeline::gif_transfer` copies each transfer into a new `std::vector`, and XGKICK has already assembled the packet in a 64 KiB buffer. That is about 880,000 PATH1 packets by vblank 2300. | Measure in P0. If visible, reuse buffers or a per-batch arena. Behaviour does not change. |
| LTO and PGO for the host | Architecture §12.5 lists them as candidates. | After AOT. Interpreter-only paths (VU0, misses) gain little. |

## Phases and sessions

### P0: Measure (one session, time box one day, coordinator)

It uses temporary instrumentation kept as a patch in `work/vu1/` and never committed, like M4's bring-up patches.
Its findings go into "What exploration established" (counts and addresses only).

1. **Profile again** with the current release build: at least 300 gdb samples (no `perf`, owner 2026-10-24), plus a
   host-time counter around `VuUnits::run` and the GS hash. This settles the non-VU1 share and the factor VU1 needs.
2. **The census numbers** over the probe, the opening scene (vblank 3600) and a menu open and close:
   - distinct programs by (entry pc, closure), and their sizes in pairs;
   - runs, cycles and XGKICKs per program, and the share of the top ten;
   - MSCAL entries against MSCNT entries;
   - branches, and JR/JALR sites with their observed targets;
   - D/T bits;
   - flag reads (FMAND, FMEQ, FMOR, FSAND, FSEQ, FSOR, FCAND, FCEQ, FCOR, FCGET), and whether any program reads
     sticky status;
   - FDIV and EFU use;
   - XGKICKs issued while a transfer runs, and whether the other pipelines were drained at that point.
3. **Where the bytes come from:** the guest source address of each MPG, and whether it lies in the boot ELF, an overlay
   image (equal bytes) or a buffer read from DATA.DAT. This decides whether the published list (remedy (a), the owner's
   choice) is possible; if it is, the coordinator drafts its ADR for the checkpoint.
4. **Timing states:** distinct entry timing states per basic block, over the observed paths. This gives the version
   counts and the cap.
5. **Owner checkpoint:** the numbers, then a go or a redesign. Many hundreds of programs, version explosion or frequent
   stalls on undrained state would change the design before day 0.

### Day 0 (coordinator, after P0 and the owner's go)

| Area | Files | What it fixes |
|---|---|---|
| ADRs | `Docs/adr/0019-vu1-aot-companion-library.md`; the published program list's ADR if P0 allows it | 0019 accepted or amended at the checkpoint; the list's format in `Docs/formats/` |
| Shared ISA and arithmetic | `src/runtime/vu/isa/`, `include/dq8/abi/vu1/vu_arith.h` | Moved out of `vu_core.cpp`, `vu_upper.cpp` and `vu_lower.cpp` without behaviour change (the probe's `--json` is byte-identical); the ISA digest. Files adapted from PS2Recomp keep their GPL-3.0-only header and attribution |
| Library ABI | `include/dq8/abi/vu1_program.h` | Descriptor, program table, run context, resume record, return codes, version 1 |
| Interpreter seams | `vu_interpreter.h` | The coverage hook, `export_snapshot`/`adopt` (declared; session B implements them), the timing-trace observer for the compiler |
| Options | `src/host/options.*` | `--vu1 interp\|aot\|verify`, `--vu1-library`, `--vu1-census`, `--vu1-census-samples`, `--vu1-selftest`, `--profile`; refused until wired (the `unwired_m*_option` pattern) |
| Status | `src/host/status_channel.*` | `vblank` in `fps`; the `vu1` and `profile` lines |
| Formats | `Docs/formats/vu1-census.md` | The census index |
| Build | `tools/vu1/CMakeLists.txt`, the library template, `tests/unit/m6_vu1_{compiler,runtime,core,wins}_tests.cmake` | One test list per session |

### Sessions

Each session works in its own worktree and touches only the files it owns. Before reporting, each runs dev-clang
with unit tests, asan, the CI container for ci-clang and ci-gcc, the checker (`--tree --deep`), pre-commit and
`reuse lint`, merges `main`, and lists what it could not verify.

**A: the compiler** (`tools/vu1/**`, the corpus generator, `m6_vu1_compiler_tests.cmake`).
- Delivers: the closure and CFG, the timing oracle and block versions, code generation (locals, commit order,
  flags on demand, lazy PATH1, bail-outs), the descriptor, deterministic output, and the corpus compiled at build time.
- Done when: the corpus passes on every CI job and the same input gives the same C++. The snapshot round trip and
  the fuzzer's seeded faults are B's, and are run again once A and B are both merged.

**B: the runtime** (`src/runtime/vu/**` beyond day 0, the census writer on the I/O thread, the `--vu1*` wiring,
`m6_vu1_runtime_tests.cmake`).
- Delivers: the census, library loading and matching, `VuUnits` integration, `export_snapshot` and `adopt`, `verify`
  mode, the status counters, and the differential fuzzer (committed; synthetic programs only).
- Done when:
  - a synthetic library built from the corpus runs through `VuUnits` with byte-identical stats and packets;
  - a bail-out at every boundary matches;
  - census files round-trip;
  - the M3 and M4 goldens are unchanged with the census on and off.

**C: setup, launcher and packaging** (`src/core/vu1/**`, the `vu1` setup stage, registry fields, `src/cli` vu1
commands, the launcher's Play-page parts, `packaging/**` for `dq8-vu1c`, the checker rules, the Windows library
build, the published program list (`data/vu1/`, its publishing tool and loader) if P0 allows it,
`m6_vu1_core_tests.cmake`).
- Starts on the day-0 contracts and stubs, and integrates after A and B.
- Done when: Optimize works from a clean profile on Linux and under Wine, with cancel and resume, and the AppImage and
  zip ship `dq8-vu1c` and pass the scan.

**D: the SHA-NI SHA-1** (`src/core/hash/**`, `m6_vu1_wins_tests.cmake`; the per-transfer buffers only if P0 shows
them and the owner agrees).
- Merges first (owner, 2026-10-24).
- Done when: digests are equal to the portable code on random input and on every golden, the portable path still
  runs where the CPU lacks the extension (forced off in a test), and the probe's time is recorded before and after.

### Merge order and bring-up

**Merge:** D, then B, A and C.

**Gate G1** (after V1): if the hottest program, compiled, is less than 5x faster on its own runs than the
interpreter, stop and take the options to the owner: a JIT (§5.4 stage 3), relaxed cycle accounting, or code
generation work.

| Step | Settles |
|---|---|
| V1 | The hottest program (the field's program at micro address 0) compiled and run under `--vu1 verify` through the probe; its throughput (gate G1) |
| V2 | Every census program compiled; the probe byte-identical between `interp` and `aot` (section 4, "End to end"); speed in all three configurations |
| V3 | The opening scene to vblank 3600, a menu round trip and the B5/B6 save path under `verify`; then the owner's playtest route (town, shop, battle) under `aot`, with new census entries optimized |
| V4 | Windows: the library built with llvm-mingw under Wine, hashes equal to Linux; then the owner on real Windows |
| V5 | A clean profile: setup (with the published list if P0 allowed it), Play, Optimize, Play; no terminal |

## Acceptance

1. **Speed.** The targets in the table under section 6 on the dev box, release preset.
2. **Exactness.** `--vu1 aot` against `--vu1 interp` on the probe, with `--gs null` and `--gs trace`: identical
   `--json`, `--events`, `gs_hash`, `--gs-trace` SHA-1, RAM SHA-1 at vblank 2300 and audio hash. `--vu1 verify` runs
   the probe and V3's segments with zero mismatches. The M3 and M4 goldens hold in both modes. Debug equals Release.
   Linux equals Windows (llvm-mingw).
3. **Fallback.** No library, a stale library or a program missing from it all give the same hashes, with a status
   warning or census entry. Bail-outs are counted.
4. **Verification in CI.** The corpus, the snapshot round trip and the fuzzer's seeded faults pass on every job that
   runs unit tests. The fuzzer is in the repository.
5. **Optimize.** It works on Linux and Windows from the launcher and `dq8ctl`, with progress, cancel and resume, and a
   self-test that refuses a miscompiled library.
6. **Legal.** The census, samples, generated C++ and library stay in the user data directory or `work/`. The checker
   refuses census paths. Release artifacts scan clean.
7. **Always.** CI is green on all nine jobs, and the checker, pre-commit and `reuse lint` are clean.

## Risks

| Risk | Handling |
|---|---|
| The 10x does not materialize (the 94 % figure is from before M4's speed-up; non-VU1 work may dominate after AOT) | P0 profiles first; D makes the hash several times cheaper (to be measured); gate G1 stops early with options |
| Timing states explode at block entries | The cap with bail-out at block entry; P0 counts versions; the interpreter is always correct |
| PATH1 stalls on undrained state are common | Lazy PATH1 handles stores and the program end; undrained stalls bail out; P0 counts them. If they are common, a dynamic-scoreboard version of the affected block |
| Arithmetic drifts between the interpreter and compiled code | One `vu_arith.h`, the ISA digest, `-ffp-contract=off`, no fast-math, boundary operands in the corpus and fuzzer, and the self-test on the user's machine. x87 `long double` is 80-bit on both Linux and llvm-mingw; ARM64 (binary128) is Tier 2 and must be revisited |
| Programs from the archives appear late (battle, events) and run slowly until Optimize | The census and Optimize with its one-time prompt; the published list where P0 allows |
| The user's compiler miscompiles | The self-test before registration; the interpreter as fallback |
| The generated code is large or slow to build | One translation unit per program at `-O2`; P0's sizes give the estimate; ccache when present |
| The census leaks into the repository | It lives outside the tree by default; checker rules; samples only with an explicit option |

## Owner decisions (2026-10-24)

| Question | Decision |
|---|---|
| Exactness | Cycle counts stay exact, as well as values, VU memory and PATH1 packets |
| First play | The census and Optimize, plus the published address-only program list if P0 finds the programs at fixed places on the user's disc (its own ADR at the checkpoint); no hot-loading |
| Optimize | A button on the Play page and a one-time prompt after a session that recorded new programs |
| Smaller wins | Only the SHA-NI SHA-1, as session D, merged first. Not approved now: `--gs-hash off` in window play, tree-wide `-ffp-contract=off`, installing `perf` |

Defaults kept from the draft (the owner can still change them at the P0 checkpoint): the VU1 library builds for
baseline x86-64, and a CPU-specific build comes back to the owner only with V2's measurements; P0, day 0 and four
sessions (A–D) as above.

## Protocol notes

*(Sessions append their findings here: the census index, the resume record's layout, the corpus generator, the
self-test, Optimize's events.)*

## Bring-up log

| Unknown | Local evidence | Resolution | Change |
|---|---|---|---|

## Results

*(Filled in at acceptance: speeds per segment and configuration, VU1 throughput, census counts, hit and bail-out
rates.)*
