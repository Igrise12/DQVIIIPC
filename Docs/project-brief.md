# Project Brief: DQ8-Native (working title)

| | |
|---|---|
| **Status** | Pre-alpha. Planning and workspace setup. No code yet. |
| **Last updated** | 2026-10-19 |
| **Target game** | *Dragon Quest VIII: Journey of the Cursed King*, PlayStation 2, NTSC-U (SLUS-21207) |
| **Approach** | Static recompilation of the PS2 Emotion Engine code to C++, running on a purpose-built native runtime. Not emulation. |
| **License** | GPL-3.0-or-later (decided 2026-10-19, see §9). Full text in `LICENSE`. |
| **Companion docs** | [architecture.md](architecture.md), [tech-stack.md](tech-stack.md) |

---

## 1. Vision

Anyone who owns a copy of *Dragon Quest VIII* for the PS2 should be able to point a launcher at their own disc
image and get a native PC game: a normal executable with a settings screen, sharp high-resolution rendering,
proper controller support and room for community enhancements. The project is open source, its code is
structured so that new features slot in without rewrites, and it never distributes anything that belongs to
the game's rights holders.

## 2. Background and motivation

The PS2 game runs on PC today through emulation (PCSX2). Emulation works, but it carries the whole machine
with it: a BIOS, a general-purpose GS renderer, generic timing and no knowledge of the specific game. A
native port can instead:

- ship as one self-contained application with a launcher, rather than a separate emulator plus configuration;
- replace emulated hardware with native code where the game only needs a narrow slice of the hardware;
- expose game-aware features (widescreen, texture replacement, interpolation, fast loading) as first-class
  settings instead of per-game emulator patches;
- give contributors a codebase that documents how the game actually works.

Static recompilation of console games to native code is now an established technique (N64Recomp, and several
PS2 efforts listed in §15), but PS2 recompilation is still **experimental and unfinished everywhere**. This
project is research as much as engineering, and the plan reflects that (see §12).

## 3. Goals

| # | Goal |
|---|---|
| G1 | **Native executable.** The game's own code is recompiled to C++ and built for the host. No emulated CPU at run time and no PS2 BIOS. |
| G2 | **Launcher.** A GUI to select and verify the user's disc image, run first-time setup, change settings, map controllers, view logs and start the game. A CLI exposes the same operations. |
| G3 | **Resolution scaling.** Render at a multiple of the PS2's native resolution, with selectable scaling filters and aspect ratio handling. |
| G4 | **Controller and keyboard support.** Gamepads through a standard API, hot-plug, full rebinding, per-device profiles, rumble. |
| G5 | **Extensible by design.** New features, renderer backends, regions and front ends are added through defined extension points, not by editing core code (architecture.md §17). |
| G6 | **Bring your own game.** The repository and releases contain no game code, data or BIOS. Everything derived from the disc is generated locally. |
| G7 | **Open source and community-friendly.** Clear docs, reproducible builds, CI, a contribution process that keeps copyrighted material out. |
| G8 | **Faithful by default.** The default mode reproduces the original game's behavior. Enhancements are opt-in or clearly separated from the faithful baseline. |

## 4. Non-goals

- **Not an emulator.** We do not aim to run arbitrary PS2 software. Hardware is implemented to the extent this one game needs it.
- **No distribution of game content.** No ISOs, ELFs, assets, extracted tables, generated C++ or BIOS images, in the repository or in releases.
- **No matching decompilation in the first phase.** Recompiled output is machine-generated and unreadable by design. Reading and replacing individual functions is supported (overrides, architecture.md §4.5), which leaves the door open to gradual decompilation, but it is not the plan of record.
- **No online features, anti-piracy circumvention or DRM work.** The game has none to speak of, and none will be added.
- **No mobile or console ports in phase 1.** Windows and Linux first. Other targets are listed as future work.
- **No re-mastering of art or audio assets inside this repository.** Community texture packs are supported through the texture-replacement feature, but their content lives elsewhere.

## 5. Users

| Persona | Needs |
|---|---|
| **Player** | Download, point at an ISO, press Play. Looks good, controller works, saves work. Does not want to install a compiler by hand. |
| **Tinkerer / modder** | Settings for everything, a patch and cheat mechanism, texture replacement, a documented config format. |
| **Contributor / reverse engineer** | Readable architecture, tracing and debugging tools, a way to prove a change is correct, clear rules about what may be committed. |
| **Packager / maintainer** | Reproducible builds, pinned dependencies, clear licenses. |

## 6. Target game: facts so far

Everything in this table was observed directly from the disc image supplied for this project unless it says otherwise.
The "inferred" rows are working hypotheses that the first milestones must confirm.

| Item | Value |
|---|---|
| Title / region | *Dragon Quest VIII: Journey of the Cursed King* (USA). Developed by Level-5, published by Square Enix. |
| Medium | DVD image, ISO 9660, 4,180,148,224 bytes (3,987 MiB), 68 files in 5 folders |
| Boot file | `SLUS_212.07` (`SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_212.07;1`, `VMODE = NTSC`) |
| Boot ELF | 2,960,032 bytes. ELF32 executable, MIPS (R5900 code). **Stripped: 0 symbols** (`.symtab`/`.strtab` present but empty). 14 section headers: one non-empty `PROGBITS` (`WAX`, `0x00100000`, `0x2D2680` bytes, code and data together), seven empty ones; BSS `0x003D2680`–`0x00461C80` is expressed only through the first segment's `memsz` (S1). Entry point `0x00100008`. |
| Main image in memory | Loaded at `0x00100000`, memory size `0x361C80`, so it ends at `0x00461C80` |
| Boot ELF SHA-1 | `f24bfd21c21f6454c05934224ab52cde837bc90b` |
| Boot ELF CRC-32 | `a66a08b0` |
| Code overlays | Six overlay files in `BIN/`: `BATTLE.BIN`, `CASINO.BIN`, `MENU.BIN`, `SHOP.BIN`, `TITLE.BIN`, `VIEWER.BIN` (1.1 KB to 156 KB each) |
| Overlay container | `MWo3` header, 64 bytes, little-endian: magic, `overlay_id` (CASINO 1, VIEWER 2, SHOP 3, MENU 4, BATTLE 5, TITLE 6), `load_address` (`0x00461C80` for all six), `text_size`, `data_size`, `bss_size`, `table_start`/`table_end` (absolute addresses inside the overlay's data; 3 to 6 entries), 32-byte lowercase name; the payload (text, then data) follows at `+0x40`. `file_size == 64 + text + data` holds for all six. **Semantics confirmed (S3):** the game reads only `bss_size`, `table_start` and `table_end`; magic, id, address, sizes and name are never checked. The table is a C++ static-constructor list whose thunks live in the *data* range (code exists on both sides of the `text_size` boundary), and the first 64 text bytes are `nop` padding. |
| Overlay window | The ELF declares six zero-file-size load segments at `0x00461C80`, the largest `0x27E00` bytes, so the window is `0x00461C80` to `0x00489A80`. Each segment's `memsz` equals `64 + text + data + bss` of one overlay, in order CASINO, VIEWER, SHOP, MENU, BATTLE, TITLE (that is, by `overlay_id`), so the whole file including its header is resident and code starts at `0x00461CC0`. **Loader confirmed (S3):** `0x00100330` reads the whole file by `cdrom0:` path through the file-I/O RPC into the address held at `0x003D1704`, calls an empty hook `0x00100320`, `FlushCache(0)` then `FlushCache(2)`, zeroes bss and runs the header's constructor table; `0x00169860` loads by index 0–5 through a six-pointer name table at `0x003950F0` (order TITLE, CASINO, VIEWER, BATTLE, MENU, SHOP) after `sceCdSeek` to a cached start sector (`0x003F8850`). A 12-mode state machine (`0x001693B0`, mode at `0x003D2740`) decides which overlay each mode needs, and 13 on-demand sites reload BATTLE, MENU, SHOP and CASINO. The heap starts at the window end `0x00489A80`. No self-modifying code anywhere; overlays never load overlays and contain no syscalls. Ghidra does not map the window by itself (S2); `tools/ghidra/LoadOverlay.java` loads a `BIN` into a project copy (functions found: TITLE 69, CASINO 226, VIEWER 16, BATTLE 624, MENU 258, SHOP 395). |
| Game data | `DATA.DAT` (1,804,218,368 bytes) and `DATA2.DAT` (752,971,776 bytes), each with an index file beside it: `DATA.HD6` (188,732 bytes, magic `HD6\0`) and `DATA2.HD6` (87,092 bytes). The ELF references `.HD6` once and `DATA.DAT` never, so lookups go through the index. **Index decoded (spike S4, `Docs/formats/hd6.md`):** a 52-byte header, a frequency-sorted path-fragment dictionary, variable-length-coded path records and 8-byte entries (18-bit record offset, 22-bit sector, 24-bit size in 16-byte units); 9,954 members in `DATA.DAT` (`.chr` 4,454, `.snd` 1,145, `.pak` 696, …) and 4,139 `RIFF`/`WAVE` voice streams in `DATA2.DAT`. The game finds each `.DAT` with `sceCdSearchFile` once, then reads members with `sceCdRead` at `LSN + sector`; nothing on that path transforms the data. `DMYDATA` (51,166,209 bytes) is text filler to the end of the DVD. |
| Movies | 38 `MOVIE/**/*.MVI` files in 19 pairs `X.MVI` / `X_W.MVI`; 91,322 frames (50.7 minutes) in all. **Format identified (spike S6a):** MPEG-2 program stream in 16 KiB packs, XORed with a 2,048-byte table that the game generates at run time; MPEG-2 Main profile video 512 × 448 progressive 30 fps; 16-bit PCM stereo audio as a Sony ADS stream in private stream 1 (no ADPCM); `_W` files are anamorphic 16:9 renders of the same movie. The table is generated and applied by the game's own read routine from data in the ELF (spike S6b), so the recompiled game decrypts movies itself; the `_W` variant is chosen by the game's screen-size setting. Details in `Docs/formats/mvi.md`. |
| Code discovery (Ghidra, S2) | Headless Ghidra 12.1.4 with the Emotion Engine extension finds **9,598 functions** in the main image (85.7 % of image bytes in function bodies, 1.0 % loose instructions, 98.6 % auto-named); 121 of 152 computed jumps have a resolved switch table; 993 of 1,124 indirect calls have no static target. The ELF loader maps none of the zero-file-size segments, so `tools/ghidra/MapElfSegmentTails.java` adds the `bss` and overlay-window blocks (120 function entries and 374 references land in the window). Import and analysis take 2–3 minutes. |
| Output and update rate (S7) | NTSC 640×448 CRTC, 59.94 Hz **field-interlaced (FIELD mode)** throughout; the game renders 512×448 into a PSMCT24 display buffer blitted from a PSMCT32 target, with a 50/50 field blend through PMODE (ALP = 127). **Game update rate 30 Hz**, 60 Hz in the main menu. First-run settings offer *Screen Size: Normal 4:3 / Wide Screen 16:9*, a native anamorphic mode on the same buffer and CRTC. Pad: DualShock 2 with analog locked on, vibration on, no pressure. PCSX2's own game CRC is `F4715852` (not the zlib CRC-32 above). |
| Boot timeline (S7, PCSX2 at 100 %) | IOP modules load in the order SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, MODHSYN, MODMIDI, MODMSIN, CDVDSTM, SKSOUND, SKHSYNTH, SKMIDI, SKMSIN, PCMPLAY (`MSIFRPC.IRX` is never loaded). Memory-card check at ≈12 s, first-run settings at ≈16 s, then two logo movies and an in-engine flyover; **title screen 64.6 s after launch** (about 16 s after the settings when the logos are skipped). `TITLE.BIN` is resident at the settings screen. VU0 microcode is loaded by the title but VU0 data memory is still zero there. |
| Instruction census (S2) | 640,430 instructions. 78 `syscall` sites with 65 distinct numbers (54 positive, 11 negative interrupt-context variants). VU0 micro mode is used at **2** `vcallms` sites; VU0 macro mode heavily (1,234 COP2 ops plus 600 `lqc2` / 354 `sqc2`); MMI 565 ops; 128-bit `lq`/`sq` 38,148 (6 % of all instructions). Also 3 `eret` and 2 `tlbwi` in a user-mode ELF (to be examined in M2). |
| IOP modules | 16 `.IRX` files plus `IOPRP300.IMG` under `MODULES/`. Standard Sony modules (`LIBSD`, `SDRDRV`, `MCMAN`, `MCSERV`, `PADMAN`, `SIO2MAN`, `CDVDSTM`, `MODHSYN`, `MODMIDI`, `MODMSIN`, `MSIFRPC`) plus game-specific ones (`SKSOUND`, `SKMIDI`, `SKHSYNTH`, `SKMSIN`, `PCMPLAY`). The `SK*` and `PCMPLAY` modules appear to form the game's own sound driver stack. *Inferred from names.* |

**What this tells us:**

1. The **stripped ELF** means no function names or boundaries come for free. Function discovery needs a disassembler pass (Ghidra) before recompilation.
2. The **six overlays sharing one address window** are the single most important design constraint. General-purpose recompilers assume one flat program. Our dispatch layer must know which overlay is currently resident (architecture.md §4.3).
3. The **sound stack is custom IOP code**, so audio needs either execution of the original IOP modules or an HLE of their interfaces (architecture.md §7).
4. **Archives and movies are understood** (spikes S4, S6a, S6b): the index layout and read path, the movie container, and the game's own XOR routine (the recompiled game undoes it, so the runtime serves raw bytes and HLEs the `sceMpeg` library). Member formats inside `DATA.DAT` are not needed to boot.

The hashes above come from a single dump. Before they are pinned as the canonical values, cross-check them against a
Redump entry for SLUS-21207. Until then they are recorded, not trusted.

**Region scope.** NTSC-U only for 1.0. PAL and NTSC-J builds will need their own function maps, overlay tables and hashes.
Region-specific data lives in region profiles (architecture.md §13) so that adding a region does not touch runtime code.

## 7. Feature plan

Priority key: **P0** required for the first public release, **P1** targeted for the releases after it, **P2** ideas to
evaluate. Feasibility notes are honest: several items depend on research that has not been done.

### Launcher and setup

| Feature | Priority | Notes |
|---|---|---|
| Disc image selection and verification (hash check, region gate) | P0 | Rejects unknown or modified images instead of mistranslating them |
| One-click first-time setup (extract, analyze, recompile, build) with progress and logs | P0 | Local build; see architecture.md §3 and the distribution decision in §14 below |
| Settings UI generated from a settings schema | P0 | New features add settings without touching launcher code |
| Controller mapping UI, profiles, hot-plug | P0 | |
| Diagnostics export (system info, logs, config) | P0 | |
| Update check | P1 | Opt-in, one anonymous request, off by default |
| Multiple game builds and profiles side by side | P1 | |
| Launcher and settings localization | P2 | |

### Display

| Feature | Priority | Notes |
|---|---|---|
| Internal resolution scaling (1x up to the maximum the backend supports) | P0 | With paraLLEl-GS: 2x–16x super-sampling plus a real double-resolution picture through backbuffer promotion and super-sampled textures, verified on this game's frames (S5b); see architecture.md §6 |
| Windowed, borderless, exclusive fullscreen, VSync, frame cap | P0 | |
| Aspect handling: original 4:3, stretch, pillarbox, integer scaling | P0 | |
| Final-image scaling and sharpening (for example FSR 1 with RCAS) | P1 | |
| Anti-aliasing options, de-interlacing choice | P1 | |
| CRT and scanline filters | P2 | |
| True widescreen (16:9 and ultrawide) | P1 | **Largely native.** The game has a *Screen Size: Normal 4:3 / Wide Screen 16:9* setting (S7); the same byte selects the anamorphic `_W` movies and is passed to the 3D projection set-up (S6b), so 16:9 is a native mode to expose rather than a patch to write. Remaining research: HUD behaviour in that mode, and ultrawide beyond it |
| Frame interpolation beyond the game's native frame rate | P2 | **Research needed.** The game updates at 30 Hz (menu 60 Hz; S7), so 60 Hz is the target. Needs scene state, not just GS packets; likely requires the native renderer path |
| Texture dump and replacement, PCSX2-style pack compatibility | P1 | A community HD texture and UI pack for PCSX2 has been reported, so compatibility is attractive |

### Input

| Feature | Priority | Notes |
|---|---|---|
| Gamepad and keyboard, DualShock 2 layout mapping | P0 | |
| Per-device profiles, hot-plug, rebinding, dead zones | P0 | |
| Rumble | P1 | |
| Pressure-sensitive button emulation | — | Dropped: the game configures the DualShock 2 with analog locked and no pressure mode (S7) |

### Audio and video

| Feature | Priority | Notes |
|---|---|---|
| Native audio output with volume control and device selection | P0 | |
| Full-motion video playback | P0 | Container, video and audio understood (S6a, `Docs/formats/mvi.md`); depends on S6b for the XOR-table source and the player interface |
| Audio latency control | P1 | |

### Game and quality-of-life

| Feature | Priority | Notes |
|---|---|---|
| Save data on disk as a plain folder, one slot set per profile | P0 | Memory card implemented as host files |
| Import of PS2 memory card images | P1 | |
| Zero-latency "fast loading" disc mode, fast-forward | P1 | |
| Memory patches and cheats, including import of common community patch formats | P1 | |
| In-game overlay menu | P1 | |
| Screenshots | P1 | |
| Mod loading (asset and patch folders) | P2 | |
| Save states | P2 | **Research needed.** Recompiled code keeps state on the host stack |
| Steam Deck and handheld profile | P2 | |

## 8. Platforms

| Tier | Platform | Notes |
|---|---|---|
| **1** | Linux x86-64 | Primary development platform (the development machine runs Arch-based CachyOS). |
| **1** | Windows 10/11 x86-64 | MSVC is the most-tested compiler for PS2Recomp. |
| **2** | Linux ARM64 | Needs NEON equivalents of the SSE paths. After 1.0. |
| **3** | macOS (Apple Silicon) | Blocked on a Vulkan layer that satisfies the renderer's feature requirements. Unscheduled. |

GPU baseline: a Vulkan 1.3 capable desktop GPU. Reference performance machine: a mid-range 2020s desktop
(6 cores, RTX 4060 class GPU). Full-speed play at 1x on that machine is a 1.0 requirement.

## 9. Legal and licensing

This section records intent. It is not legal advice, and it should be reviewed by someone qualified before the first public release.

**Bring your own game.**
- Releases and the repository contain **only first-party code and third-party libraries**. They contain no disc contents of any kind.
- Everything derived from the disc (extracted files, analysis results, recompiled C++, the built game module) is generated on the user's machine and is git-ignored.
- The host runtime is distributed as a normal binary. The **game module**, which is the recompiled game code, is built locally from the user's disc and is never uploaded or distributed. This split is a core architecture decision (architecture.md §2).

**What may be committed.** Addresses, sizes, names and function boundaries that we authored, expected hash values,
and shape-only tables (labels, sizes, pointer slots, no values). **What may not:** instruction bytes, data values,
text, image or audio content from the game, generated code, or anything extracted from the disc. A pre-commit hook
and a CI check enforce this (architecture.md §14).

**No BIOS.** The native runtime does not use a PS2 BIOS, so no BIOS is needed or accepted.

**Project license: GPL-3.0-or-later** (decided 2026-10-19; ADR-0009; full text in `LICENSE`). PS2Recomp, the
recompiler and runtime foundation we expect to build on, is GPL-3.0. Linking its runtime makes the combined work
GPL-3.0. The renderer we expect to use, paraLLEl-GS, is LGPL-3.0-or-later, which is compatible. Dependencies must be
GPL-3.0-compatible (tech-stack.md §10). A permissive license would require a clean-room runtime and recompiler,
which is not the plan. Every first-party source file carries an SPDX header (`SPDX-License-Identifier` set to `GPL-3.0-or-later`).

**Contributions.** Developer Certificate of Origin sign-off (`Signed-off-by`), no contributor license agreement.
Contributions must not include game-derived content or code from sources whose license is incompatible. Contributors
who have studied non-open-source or leaked material for this game should not contribute from it.

**Trademarks.** *Dragon Quest* and related marks belong to Square Enix. The project name, icon and documentation make clear that it is
unofficial. The working title may change (§14).

## 10. Constraints and assumptions

- A legally obtained NTSC-U disc image is required. It must match the pinned hashes.
- Setup builds the game module locally, which needs a C++20 toolchain, CMake and Ninja. Spike S1 measured the boot
  ELF on a 6-core machine: recompiling takes 2 s; compiling the 8,335 generated units costs about 15,000–16,000 CPU-s
  (30–40 minutes), about 6,000 CPU-s (≈15 minutes) with a precompiled header; 5–7 GiB of RAM at 12 jobs; about
  0.5 GB of disk. Incremental setup, the PCH, a compiler cache and a job count chosen from free RAM are design
  requirements (architecture.md §3, tech-stack.md §6); a bundled toolchain remains the Windows plan.
- PS2Recomp is experimental, and its runtime has partial hardware coverage and poor VU and GS performance. We
  will build or replace large parts of the runtime ourselves.
- No one has published a recompilation, decompilation or format documentation for this game that we could find. We
  should expect to reverse-engineer the data archives, overlay loader, sound stack and movie format ourselves.
- The project is volunteer work. Milestones below are defined by exit criteria, not dates.

## 11. Milestones

Each milestone ends with a demonstrable result. A milestone is done only when its exit criteria pass on a clean checkout.

| # | Milestone | Exit criteria |
|---|---|---|
| **M0** | **Foundations** | **Done 2026-10-20.** Workspace, docs, CI, all eight spikes (S0–S7) with findings; D1–D5 decided (ADRs 0003, 0004, 0008, 0009); D6 deferred to M5; D7–D9 pending. |
| **M1** | **Disc and binary intake** | **Done 2026-10-20.** `dq8ctl` reads the ISO, verifies the boot hash and the ELF against the region profile, extracts the ELF, overlays and IOP modules idempotently, and parses and cross-checks the six `MWo3` overlay containers; 39 unit and 4 local tests. (The archive index was spike S4's deliverable, not an M1 gate.) |
| **M2** | **Analysis and first recompile** | **Done 2026-10-20.** Ghidra function maps for all seven units; `ps2_recomp` (built from the pinned submodules) emits 0 unhandled instructions; seven game modules build (main in about 7 minutes on 6 cores); `dq8-game` runs from `0x00100008` to the first syscall, `SetupThread` (`Docs/milestones/M2-first-recompile.md`). |
| **M3** | **Boots** | Memory, kernel HLE, threads, CDVD and overlay switching work. Game reaches the title screen. First pixels through the GS backend. |
| **M4** | **Playable core** | Pad, memory card, audio, VU1 and movies work. A new game can be started, a battle fought (battle overlay), the menu and shop used, and a save made and reloaded. |
| **M5** | **Launcher MVP** | First-run wizard, verification, setup with progress, settings UI, controller mapping, resolution scaling. A new user can go from ISO to playing without a terminal. |
| **M6** | **Full playthrough** | The entire game is completed on Linux and Windows at full speed without game-breaking defects. |
| **M7** | **1.0 release** | Packaging, documentation for players, CI-built releases, issue templates, security and legal review done. |
| **Post-1.0** | Enhancements | Widescreen, texture replacement, interpolation research, PAL and NTSC-J, other platforms, mods. |

**M0 spikes** (small, time-boxed experiments that de-risk the plan before real work starts; briefs and findings live in `Docs/spikes/`, and S0 checks whether the REA tooling can open our ELF):

| Spike | Question |
|---|---|
| S1 | Does PS2Recomp build with the Linux toolchain on the development machine (GCC and Clang), and how long does it take? **Done:** builds unpatched with both (445/445 tests); recompile 2 s; compile cost measured (§10); the recompiler ignores the overlay segments, so overlays need per-unit synthetic ELFs. |
| S2 | Can Ghidra with an Emotion Engine plugin load `SLUS_212.07` and export a usable function list? Does the exporter handle a stripped ELF of this size? **Done:** yes, headless, in 2–3 minutes; 9,598 functions; the exporter runs unmodified with a `.properties` answer file. |
| S3 | How does the overlay loader work? Find the function that reads `BIN/*.BIN` and the instruction sequence that makes new code live (cache flush). **Done:** loader, index table, mode machine, constructor table, flush sequence and the no-SMC verdict are in the S3 findings; M1 part 2 is unblocked. |
| S4 | What is the `DATA.HD6` index layout, and how does the game read `DATA.DAT` through it? **Done:** layout, reader and read path in `Docs/formats/hd6.md`. |
| S5 | Can paraLLEl-GS be built standalone and fed a GS dump captured from PCSX2, with the output matching? **S5a done:** builds unpatched with GCC and Clang, the RTX 4060 meets every requirement, the embedding API is the `GSInterface` class. S5b (replay of real dumps) waits for S7. |
| S6 | What are the `MVI` movies (MPEG-2 through the IPU, or a custom codec)? **S6a and S6b done:** XOR-obfuscated MPEG-2 program streams with PCM audio; the game's read routine generates the table and applies the XOR, and the player is mapped down to its `sceMpeg` library calls (`Docs/formats/mvi.md`). |
| S7 | What frame rate and resolution does the game run at in PCSX2, and which GS features does it use heavily? **Done** (battle scene still missing): 30 Hz update (menu 60) at 512×448 field-interlaced; native 16:9 option; GS histograms, raw dumps and save states captured with the first-party headless harness. |

## 12. Risks and mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **Overlays break the flat-program assumption** of the recompiler and runtime | Certain, now understood (S1, S3) | High | Overlay-aware dispatch designed in from day one; each overlay recompiled as a separate unit from a synthetic ELF with its own function map and prefixed dispatch table (architecture.md §4.3). Residual, for M2: per-site attribution of 39 ambiguous main call sites and function discovery inside MENU (53 % coverage) |
| R2 | **Stripped ELF**: incomplete or wrong function discovery causes crashes at run time | Medium (was High; S2 found 9,598 functions covering 85.7 % of the image) | High | Ghidra analysis, run-time fallback that logs unresolved addresses (the residual risk is 31 unresolved switch tables and 993 indirect calls), iterative triage loop, overrides for hard cases |
| R3 | **GS rendering speed and accuracy** | Medium (was High: S5b replayed this game's frames in paraLLEl-GS at SSIM 0.992–0.997 against PCSX2, 5 ms of GPU per field frame at 1x and 18.5 ms at 16x on the RTX 4060) | High | Renderer behind an interface; paraLLEl-GS first, native renderer later (architecture.md §6). Residual: about 8 ms of CPU GIF decode per field frame (the GS thread is mandatory), low-end GPUs, and the battle scene, which no dump covers yet |
| R4 | **VU1 microprogram performance** | Medium | High | Interpreter first for correctness, then ahead-of-time compilation of programs found at run time (architecture.md §5.4) |
| R5 | **Custom IOP sound stack** (`SK*`, `PCMPLAY`) | High | Medium | Bootstrap by executing the original IOP modules, replace with HLE once behavior is understood (architecture.md §7) |
| R6 | **Unknown archive and movie formats** | Low (solved by S4, S6a and S6b) | Medium | Format docs under `Docs/formats/`, patterns under `tools/extract/`. Residual: member formats inside `DATA.DAT` (`.chr`, `.pak`, `.snd`, …), which the port does not need to parse to run the game |
| R7 | **Timing, threads and interrupts**: recompiled code cannot be pre-empted at arbitrary instructions | Medium | High | Explicit yield points and a deterministic scheduler (architecture.md §12); tracing support |
| R8 | **FPU semantics**: the R5900 FPU is not IEEE-754 (round-toward-zero, no NaN or infinity) and small differences can desynchronize or hang the game | Medium | High | Dedicated arithmetic helpers, controlled host rounding mode, differential tests (architecture.md §15) |
| R9 | **Build time** for users (disk is not a concern: about 0.5 GB) | Medium (measured by S1: 30–40 min on six cores, ≈15 min with a precompiled header, 5–7 GiB RAM) | Medium | Precompiled runtime headers (0.37× per unit), `-O2` for function units and `-O0/-O1` for the dispatch table, splitting the few multi-megabyte units, job count from free RAM, compiler cache keyed on the deterministic output, incremental setup |
| R10 | **Legal exposure** from accidental inclusion of game data | Low | Very high | BYO-disc design, pre-commit and CI checks, contribution rules (§9) |
| R11 | **Upstream churn**: PS2Recomp moves quickly (16 commits in the last 90 days from 7 authors) but builds and tests cleanly (S1: zero patches needed) | Medium | Low | Pin to `eb8688b`, keep a small documented patch queue (empty today), contribute fixes upstream |
| R12 | **Scope**: enhancements distract from getting the game to boot | Medium | Medium | Strict milestone order; feature work starts only after M4 |
| R13 | **Single-contributor bus factor** | Medium | Medium | Written architecture, ADR log, tests, no private knowledge |

**Calibration.** No one has published a complete, playable PS2 native port through this method for a game of this
scale. The reference projects in §15 range from "boots and plays with sound but nobody has finished it" to "archived". The plan
is built to deliver value in stages (a verifiable intake pipeline, a function map, a runtime other projects can reuse)
even if full-speed playthrough takes longer than hoped.

## 13. Success criteria for 1.0

1. A user with a matching NTSC-U disc image goes from download to playing without using a terminal.
2. The game can be played start to finish on Windows and Linux without game-breaking defects.
3. Full speed (1.00x) at 1x resolution on the reference machine, and at 2x or higher on a mid-range GPU.
4. Resolution scaling, aspect-ratio handling, controller rebinding and save handling all work from the launcher.
5. A contributor can build from a clean clone using the documented steps, and CI passes without any game data.
6. A scan of the repository and release artifacts finds no game-derived content.

## 14. Open decisions

Decisions that need an answer from the project owner. Defaults are proposed so work can proceed, but each is recorded in the ADR log (architecture.md §18) once decided. D1 and D2 were decided on 2026-10-19, D3, D4 and D5 on 2026-10-20; D6 to D9 are still proposals.

| # | Decision | Decision or proposed default | Why it matters |
|---|---|---|---|
| D1 | **Project name** | **Decided:** keep the working name **DQ8-Native**. It can still change before the first public announcement. | Trademark and discoverability |
| D2 | **License** | **Decided:** GPL-3.0-or-later (ADR-0009). `LICENSE` added. | PS2Recomp is GPL-3.0; changes what can be linked |
| D3 | **How the game module is produced** | Local build on the user's machine from their disc (first-run setup); system compiler on Linux, bundled toolchain planned for Windows. **Decided 2026-10-20 (ADR-0008).** | Legally safest; costs 15–40 minutes of first-run compile. The alternative of shipping pre-built recompiled code is not acceptable under §9. |
| D4 | **Relationship to PS2Recomp** | Pin `eb8688b` plus a patch queue (empty today), write our own runtime on top. **Decided 2026-10-20 (ADR-0003).** | A fork diverges quickly; the per-code-unit changes we need are small and local to `ps2xRecomp` |
| D5 | **First GS backend** | paraLLEl-GS (Vulkan), with a native HLE renderer later. **Decided 2026-10-20 (ADR-0004).** | Determines the resolution-scaling story and GPU requirements |
| D6 | **Launcher toolkit** | Dear ImGui over SDL3 in phase 1, behind a front-end abstraction | One toolkit shared with the in-game overlay; can be replaced later |
| D7 | **Which region profile ships first** | NTSC-U only | Test coverage and hash pinning |
| D8 | **Hosting and community** | Public Git repository with issue tracker. Chat server later. | Needed before M7 |
| D9 | **Function-map publication** | Keep the Ghidra function map local and regenerate it from the user's disc with `tools/ghidra/headless.sh` (about 2 minutes) until the legal review in §9; do not commit `config/functions/` in M0–M2. Proposed by spike S2. | The map is 98.6 % auto-names plus addresses and churns with every analysis change; committing it buys nothing yet and is the most derived artifact we would publish |

## 15. Prior art and references

These projects were studied while preparing this brief. Lessons are summarized because they directly shape the architecture.

| Project | What it is | Lessons we take |
|---|---|---|
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) (ran-j, GPL-3.0) | Static recompiler and runtime tool for PS2 games. Modules: analyzer, recompiler (R5900 to C++), runtime, IOP module executor. Uses Ghidra export for stripped ELFs. | **Our recompiler foundation.** Documented workflow: Ghidra export, TOML config with stubs, skips and instruction patches, generated C++ linked to a runtime. Its per-build "game override" mechanism for rebinding functions is the model for ours. Marked experimental, with poor VU and GS performance. |
| [Tenkaichi3Decomp](https://github.com/z3xox/Tenkaichi3Decomp) | Native PC port built on a matching decompilation of *Budokai Tenkaichi 3*. Vulkan renderer with OpenGL fallback, resolution scaling, rebinding, texture packs. | Keep the port layer separate from game code; ship no assets and keep tables shape-only; keep simulation deterministic and decoupled from rendering; reproducible release builds in Docker and CI; a port status log separate from game notes. |
| [rac1-native-game](https://github.com/daino-selvatico/rac1-native-game) (MIT) | Native port of *Ratchet & Clank* (PS2). Function-by-function translation checked against the original by a differential test harness. OpenGL 4.6 renderer. | Differential ("oracle") testing; control FPU rounding; keep the renderer behind an interface; region table of addresses and shifts instead of copied data; a setup script that verifies the disc and generates everything locally; split docs for players and contributors. |
| [sly1](https://github.com/theonlyzac/sly1) | Matching decompilation of *Sly Cooper and the Thievius Raccoonus* (PS2). Splat, objdiff, Ninja. | Function-level matching with fallbacks so the build always runs; checksum gate on output; quickstart script that extracts the ELF from the user's ISO; container setup guide. A reference if we later pursue decompilation. |
| [ico-recomp](https://github.com/nathanialf/ico-recomp) (MIT, archived) | Static recompilation of *ICO* (PS2, PAL) with VU1 microprograms also recompiled, SDL host layer, paraLLEl-GS shipped as the renderer, in-game menu. | SHA-1 gate on the boot ELF; paraLLEl-GS as a shared library to respect LGPL; generated code gitignored and a `no ROM` check as pre-commit and CI step; a clean-room renderer kept hidden until it passes a parity gate. Archived 2026-10-07, which is a reminder of project-sustainability risk. |
| [god-of-war-recomp](https://github.com/KIexster/god-of-war-recomp) (GPL-3.0) | *God of War* (PS2) through PS2Recomp plus a native runtime that replaces PS2 hardware with native code (GS, VU1, IOP, sound, DMA, disc). | The practical shape of a working PS2 port: replace hardware with native code instead of emulating it; compile VU1 microprograms ahead of time; pin PS2Recomp and apply an ordered patch queue; keep the Windows path short; budget 20 to 40 minutes and about 20 GB for a clean build. |
| [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) ([blog](https://themaister.net/blog/2024/07/03/playstation-2-gs-emulation-the-final-frontier-of-vulkan-compute-emulation)) | PS2 Graphics Synthesizer emulation with Vulkan compute shaders, with 2x to 16x supersampling. LGPL-3.0-or-later. | Candidate first GS backend: accurate and already supports resolution scaling. Requires specific Vulkan features (descriptor indexing, timeline semaphores, 8 and 16-bit storage, subgroup size control). Documents no embedding API, so we expect to wrap it ourselves. |
| [PCSX2](https://github.com/PCSX2/pcsx2) | The reference PS2 emulator. | Ground truth for behavior and a source of captures (GS dumps) and a debugger. Also where community patches and texture packs are produced. |
| [BFM-decomp](https://github.com/Druthulu/BFM-decomp) | Byte-matching decompilation of *Brave Fencer Musashi* (Square, PS1). | Same publisher as our target. Not yet reviewed in detail; worth a look for file-format and documentation conventions. |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) | The recompiler that inspired PS2Recomp. | Established pattern: recompiler tool, runtime library, game-specific project that ships no assets. |
| [PS2 Remaster Pack for *DQ VIII*](https://wccftech.com/dragon-quest-viii-ps2-remaster-pack-introduces-massive-ui-and-texture-improvements/amp/) | Community HD UI, font and texture pack for PCSX2. | Evidence of demand for texture replacement; a compatibility target for P1. |

An actual native port of this specific game was not found during research. Searches for decompilation or
reverse-engineering projects for *Dragon Quest VIII* (PS2) on GitHub returned none, which also means no existing
function maps or format documentation.
