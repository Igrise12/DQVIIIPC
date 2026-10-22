# M4: Playable core

| | |
|---|---|
| **Status** | Planned 2026-10-21; day 0 (contracts, the VU0 bridge) on `main` 2026-10-21; sessions A–D next |
| **Exit (brief §11)** | Pad, memory card, audio, VU1 and movies work; a new game can be started, a battle fought (BATTLE overlay), the menu and shop used, and a save made and reloaded |
| **Owner choices (2026-10-21)** | Audio: the disc's sound IRX modules on an IOP interpreter with a register-level SPU2 (ADR-0014) · movies: sceMpeg HLE with a dynamically loaded libavcodec (ADR-0015) · saves: host folder only (ADR-0016) · four parallel sessions, then serial bring-up · the coordinator scripts the playthrough · speed recorded, not gated (VU1 stays on the interpreter) |
| **Proposed, for the owner** | ADR-0013 (guarded guest-state writes, the flyover timeline skip) |

Sessions read this file. Contract changes go through the coordinator. Every rule in CLAUDE.md applies; the most
important is that **no game-derived content** goes into commits: addresses, sizes, counts, SDK names, server ids,
ordinals and our own authored data are fine; bytes, disassembly, decompiled code, extracted files, frames, audio
and saves are not.

## What exploration established

**The path to the field (day-0 probe, `dq8-game` on the disc).**
- Title menu → Create → name entry → "save the adventure log?" No → mode 0 at vblank 1688 → **BATTLE made resident at
  vblank 1699** through the residency hook. PCSX2's `field.p2s` (mode 0, overlay id 5 BATTLE) and `menu.p2s` (mode 0,
  id 4 MENU) agree: the field keeps BATTLE resident, and the main menu swaps MENU in.
- Before day 0 the run stopped at vblank 1786 on `VCALLMS` at `0x0013CC7C`. The only VCALLMS sites are
  `FUN_0013CC70` and `FUN_0013CCE0` (main, both start address 0). No unit writes CMSAR1; one site writes FBRST; three
  read VPU_STAT.

**The sound modules (import census over the disc's IRX files, names and ordinals only).**

| Library | Ordinals the ten sound modules import |
|---|---|
| intrman | 4, 5, 6, 7, 17, 18, 23 |
| loadcore | 5, 6, 7 |
| sifcmd | 14, 15, 16, 17, 19, 22, 24, 25 |
| sifman | 7, 8 |
| stdio | 4 |
| sysclib | 8, 12, 14, 17, 27, 29, 36 |
| sysmem | 4, 5, 14 |
| thbase | 4, 5, 6, 10, 14, 20, 22, 24, 26, 28, 33, 34, 36, 37, 38, 39, 40 |
| thevent | 4–10 |
| thsemap | 4–8 |
| timrman | 4, 6, 20, 22, 23, 24 |

- 66 HLE functions in all. Guest-to-guest links: libsd (26 ordinals), modhsyn (14), modmidi (16), modmsin (4),
  sksound (10), skhsynth (3), sdrdrv (1). CDVDSTM would add cdvdman {11, 12, 39, 40, 46, 48, 49, 50, 61, 114} and
  ioman {20, 21}, but no sound module imports it.
- Module code is user-mode MIPS I integer code only (no COP0, COP2 or unaligned-access instructions). Only LIBSD
  touches hardware: the SSBUS delay registers, IOP DMA channels 4 and 7, DPCR/DICR/DPCR2, and the SPU2 map.
- The IOP update image `IOPRP300.IMG` carries SYSMEM, LOADCORE, SIFCMD, SIFMAN, THREADMAN, IOMAN, MODLOAD, FILEIO,
  CDVDMAN, CDVDFSV, LOADFILE, TIMEMANI, ROMDRV, EESYNC, SYSCLIB and STDIO; intrman and timrman are ROM modules. All of
  them are HLE for us.
- SDRDRV imports sifcmd BindRpc and CallRpc: it is also an **IOP-to-EE RPC client** (the EE's sceSdRemote callback
  server). PCMPLAY's libsd imports read as block transfer, so movie audio needs the SPU2's **AutoDMA** (S6b's "voice
  transfer" reading looks off by one; to confirm in bring-up).

**The movie player (main unit; S6b plus the design review).**
- The open routine `0x001A1C60` configures IPU DMA channel 4 but never starts it outside the SDK's decode path.
- The game's video stream callback `0x001A06C0` copies demuxed video into a 32 KiB staging ring that only the IPU path
  drains; the decoder loop `0x0019E840` reads mp+0/+4/+8 (width, height, the index of the picture delivered) after
  GetPicture; the GIF builder `0x0019E5F0` walks 1 KiB tiles column-major; stop `0x001A13D0` terminates the player
  threads, so host frames inside the HLE must be abandonable.

**Upstream and reference code.** PS2Recomp (`eb8688b`): ps2xIOP (R3000A interpreter 497 lines, IOP kernel 744, IRX
loader 561, sysclib 336, timrman 476; tests about 1,650), the VU0 micro bridge (`ps2_runtime.cpp:198-272`, adapted on
day 0), `Kernel/Stubs/MPEG.cpp` (about 300 of 2,573 lines useful), `Kernel/Stubs/MemoryCard.cpp` (path
normalisation, wildcards, attributes). PCSX2's SPU2 behaviour may be adapted (GPL-3.0-or-later, attributed). The dev
host has libavcodec 63 (FFmpeg 8); Ubuntu 24.04 CI has 60.

**PCSX2 references on hand.** `work/pcsx2/new-game-field-20261019-204003/field.p2s`, `menu-20261019-203216/menu.p2s`
and their GS dumps. Save states hold `iopMemory.bin`, `SPU2.bin` (sound RAM and registers), VU memories and EE RAM.
Battle has never been captured.

## Design rules every session follows

- **Virtual time only.** Nothing the guest observes depends on wall time. SPU2 → ring → sink and decoder → pixels are
  one-way. Completions are events at `now + cost`.
- **No new event before first use.** With no interpreted module, no movie and no card activity, the runtime schedules
  no IOP, SPU2 or movie event: M3's output and the synthetic goldens stay identical.
- **IOP code runs only from its slice event**, never inline from an EE call, an MMIO access or another device.
- **Stop instead of a silent default:** unknown imports, IOP MMIO, IOP DMA channels, sysmem modes, interrupt lines,
  relocations, IPU decode commands, mcserv functions not yet seen.
- **No order-dependent containers** in behaviour paths; no host pointers in guest or IOP RAM.
- **Licences:** upstream code is GPL-3.0-only with the PS2Recomp attribution; PCSX2 code is GPL-3.0-or-later with
  the PCSX2 Dev Team attribution; ps2sdk (AFL-2.0) is a protocol reference only; game evidence stays in `work/`.

## Contracts (day 0, on `main`)

| Area | Files | What it fixes |
|---|---|---|
| VU0 micro mode | `vu/vu0_runner.h`, `vu/vu0_bridge.{h,cpp}`, `VuUnits::run_from_ee`, `MachineServices::vu0()/set_vu0()` | VCALLMS/VCALLMSR run unit 0 on the calling context's VU0 registers (copied in and out, adapted from upstream); the program's own cycles are charged to the EE clock. VIF0 MSCAL keeps the unit's register file until session D shares the views |
| Native-to-guest calls | `ee/overrides.h` (`IGuestCalls`, `OverrideCall::call_guest/scratch/kernel_call`), `Dispatcher::set_guest_calls`, `EeRuntime::call_function/call_kernel` | An override calls a guest function or a kernel service by name on the thread's fiber; the callee may block or be terminated with its thread |
| Clock and cost | `machine/clock.h`, `machine/cost_model.h` ("m4-v1") | One IOP cycle = 8 EE cycles; one SPU2 sample = 6,144 EE cycles; the M4 cost fields below |
| Trace categories | `machine/machine_services.h`, `--trace` | `iop`, `spu2`, `audio` |
| IOP hardware seam | `iop/hw/iop_hw.h` | `IopAccess`, `IIopBusDevice`, `IopIrq`, `IopDmaRequest/IopDmaStart`, `IIopDmaDevice`, `IIopHardwareHost`, `IIopHardwareClient`, `IopDeviceMapping` |
| IOP configuration | `iop/iop.h` (`IopConfig`), `iop/iop_modules.h` (`IopServiceDeps`) | `devices`, `strict_imports`, `cards[2]`, `rtc`, `pad_feedback` (forward-declared types; D writes their headers) |
| SPU2 stub | `spu2/spu2.{h,cpp}` | Attaches, schedules nothing, stops on any access; `mapping()` for `IopConfig::devices` |
| Audio output | `platform/audio.{h,cpp}` | `StereoFrame`, `AudioRing` (SPSC), `IAudioTap`, `AudioOutput` (audio hash), `IAudioSink`, null sink; `make_wav_tap` declared |
| Movies | `include/dq8/movie/movie_decoder_abi.h`, `movie/movie_decoder.h`, `movie/ipu_regs.{h,cpp}` (attached in `ee_devices.cpp`) | The dq8-movie C ABI, `IMovieDecoder`, and the IPU register stub (set-up commands accepted, decode commands stop) |
| Pad feedback | `io/pad_state.h` (`PadActuators`, `IPadFeedbackSink`), `platform/platform.h` (`set_rumble`) | Actuator changes out of the pad service, rumble into the platform |
| Host options | `host/options.{h,cpp}` | Parsed and documented: `--audio`, `--audio-dump`, `--audio-latency`, `--movies auto\|play\|nodecode\|skip\|blank\|stop`, `--movie-decoder`, `--save-dir`, `--memory-card[2]`, `--rtc`, `--rumble`, `--vu0-registers`, `--dump-dir`, `--dump-iop-ram-at`, `--dump-spu2-at`. Options whose subsystem has not arrived are refused at start-up (`unwired_m4_option` in `run_machine.cpp`, one line per session) |
| Host wiring | `host/m4_wiring.h`, `host/{audio,movie,save,feedback}_wiring.cpp` | One function per session, called before the IOP is built: devices, overrides, start/stop hooks, end-event fields |
| Input scripts and `--until` | `io/input_script.*`, `game/until.*` | Format version 2: `mark:NAME` and `event:TYPE[:FIELD=VALUE]` triggers and the `mark NAME` action (a `mark` event; with `--dump-dir` the next frame as `<script>-<name>.png`); `--until mark:NAME` and `--until event:...` |
| Profile | `include/dq8/core/region_profile.h`, `src/core/profile/region_profile.cpp`, `data/regions/SLUS_21207.toml` | `[movie.sce_mpeg]` (all eleven entries required); `services = ["interpret"]` is reserved for session A |
| Build | `src/runtime/{iop/interp,spu2/spu2,movie/movie,io/memcard}.cmake`, `tests/unit/m4_{iop,spu2,movie,save}_tests.cmake` | One source list per session; no session edits a shared CMake list |
| Test support | `tests/unit/support/fake_iop_host.h` | `FakeIopHost`: IOP RAM, a settable IOP time, recorded IRQs and DMA completions |
| Reporting | `--json` `machine.vu` | Programs, cycles and XGKICKs per VU unit |

**Cost model "m4-v1".** M3's fields are unchanged ("m3-v1" timing), so the M3 acceptance hashes hold.

| Field | Value | Use |
|---|---|---|
| `iop_instruction` | 8 | one interpreted IOP instruction (one IOP cycle) |
| `iop_hle_call` | 160 | each HLE library call, plus what the service charges |
| `iop_hle_per_byte` | 2 | sysclib copies |
| `iop_slice` | 16,384 | the longest IOP slice before the EE gets a turn (2,048 IOP cycles) |
| `iop_spin_reads` | 64 | identical device reads before the IOP spin detector jumps |
| `spu2_block_samples` | 32 | samples per output block when nothing touches the SPU2 |
| `spu2_dma_per_halfword` | 0 (session B sets it) | manual SPU2 DMA time |
| `movie_picture` | 1,179,648 (4 ms) | one picture the movie HLE delivers |
| `mc_latency_us`, `mc_bytes_per_second` | 200, 1,000,000 | memory-card replies |

**Events added in M4** (deterministic fields only): `iop_module` gains `mode`, `base`, `size`, `start`;
`iop_link {module, hle, guest, unimplemented}`, `iop_import_unimplemented {module, library, ordinal}`,
`iop_rpc_register {sid, module}`, `audio {frames, hash}` (only when frames changed), `movie {phase, name, pictures,
slot_a, slot_b}`, `movie_decoder {available, reason}`, `mc_op {op, port, path, result}`, `pad_rumble {port, small,
large}`, `mark {name}`. The `end` event gains fields through `M4Wiring::end_fields` (`audio_hash`, `card_sha1`).

## Sessions

Each session works in its own worktree, branched from `main` after day 0, and touches only the files it owns.
- If you need a contract change, ask the coordinator; do not edit another session's files.
- SPDX headers; clang-format clean (`pre-commit run clang-format --files ...` after staging); warning-free on GCC,
  Clang and MSVC (`-Werror` in CI; this clang also warns on aggregate initializers that leave fields out).
- **Tests are synthetic only**: no game data and no GPU in CI.
- Before reporting: `cmake --build --preset dev-clang`, `ctest --preset unit-dev-clang`, the `asan` preset, the CI
  container for ci-clang and ci-gcc (`localhost/dq8-ci:24.04`, mount the repo at `/build-src`), `python3 -I
  scripts/check_no_game_data.py --tree --deep`, `pre-commit run --all-files`, `reuse lint`; merge `main` into your
  branch; list what you could not verify.

### A: IOP interpreter and sound-module hosting (largest, about 7,000–8,000 lines)

**Owns:** `src/runtime/iop/**` except `services/{mcserv,padman,cdvdfsv}.cpp`; `iop/interp.cmake`;
`tests/unit/support/{irx_builder.*,sif_client.h}`; `tests/unit/m4_iop_tests.cmake` and the `test_iop_*`,
`test_irx_*` files.

**Delivers:**
- `iop/cpu/`: the R3000A interpreter (from upstream `iop_cpu.cpp`): load-delay slot, the HLE trap opcode in an HLE
  page, stops for address error, `break`, overflow and reserved instructions naming the IOP thread, pc and
  "MODULE+offset".
- `iop/loader/`: the IRX loader (ET 0xFF80, `.iopmod`, REL relocations; an unknown relocation stops) and linker (HLE
  first, then guest export tables; stubs patched to `j target`; a missing library stops at load; an unimplemented
  ordinal links to a stop trap and emits `iop_import_unimplemented`; `strict_imports` stops at load).
- `iop/kernel/`: IOP threads as register contexts (no fibers), preemptive priorities, interrupt contexts, blocking
  calls and time waits as events; HLE libraries covering every census ordinal (our own `printf` formatter into the
  `iop` trace category; timrman pure HLE; sysmem allocation modes 0, 1, 2; other modes stop).
- `iop/hw/`: the IOP bus (RAM, 1 KiB scratchpad, `IopConfig::devices`), INTC (`0x1F801070/74/78`), DMAC (channels 4
  and 7 to the mapped device; any other channel started stops), SSBUS registers as stores.
- `iop/iop_exec.*`: slices at event time, never past the next event, at most `iop_slice`; an IOP spin detector; an
  idle IOP costs nothing.
- RPC: guest servers (`add_guest`, `complete_guest_call`), deferred native replies (loadfile waits until an
  interpreted module's start returns), IOP-as-client (`client_bind`, `client_call`, `on_end`), and `Iop::on_command`
  routing `cid_rpc_end`. Layouts go into "Protocol notes" below, re-derived (ps2sdk is a reference only).
- The catalogue name `interpret` (exclusive in its `[iop]` row). Flipping the ten sound rows is bring-up work.

**Done when:** synthetic IRX modules (our builder) load, link to HLE and to each other, run threads, timers and DMA
interrupts, serve an RPC to the host SIF client and call an EE-side server; loadfile replies after module start;
the same program twice gives identical events and IOP RAM; the M3 goldens are unchanged; a coverage test lists every
census ordinal as implemented or an explicit stop.

### B: SPU2, mixer and audio output (about 5,500–6,500 lines)

**Owns:** `src/runtime/spu2/**` (replacing the day-0 stub), `spu2/spu2.cmake`, `platform/audio.cpp` beyond day 0
(the WAV tap), `platform/sdl/sdl_audio.{h,cpp}`, `host/audio_wiring.cpp`, the `audio`/`audio-dump`/`dump-spu2-at`
lines of `unwired_m4_option`; `tests/unit/m4_spu2_tests.cmake` and the `test_spu2_*`, `test_audio_*` files.

**Delivers:**
- The register-level SPU2: 2 MiB sound RAM; two cores of 24 voices; ADPCM (five filters, loop flags), pitch, 4-tap
  Gaussian interpolation, ADSR and volume sweeps, noise, pitch modulation, KON/KOFF/ENDX, ATTR and the reset handshakes
  LIBSD polls, MMIX dry/wet routing, core 0 into core 1, the reverb work area; integer arithmetic only. Behaviour from
  PCSX2's `pcsx2/SPU2/` read in a git-ignored checkout at the 2.7.524 tag (`work/ref/pcsx2`; approved with the plan).
- Clocking: `advance_to(t)` on every register access and DMA start; a tick every `spu2_block_samples` only after the
  first register write; output and state identical however `advance_to` calls are split.
- DMA: manual on channels 4 and 7 with a completion time; AutoDMA with deferred completion, pulling input blocks at
  the sample rate; IRQA hits raise IOP INTC 9.
- `AudioOutput` taps (the WAV writer), the SDL3 sink (`SDL_OpenAudioDeviceStream`, S16 stereo 48 kHz, callback on
  SDL's audio thread, rate control within ±0.5 %, reopen on device loss), `wire_audio` (output, SPU2, sink,
  `--audio-dump`, the `audio` events and `audio_hash` end field).

**Done when:** golden PCM hashes for authored ADPCM blocks, envelopes, reverb, DMA and AutoDMA, IRQs and timing
invariance; the WAV tap and the audio hash are identical across `--speed`; an SDL dummy-driver test; M3 goldens
unchanged with the SPU2 attached but untouched.

### C: Movies (about 3,000 lines)

**Owns:** `src/runtime/movie/**` (with the day-0 headers and IPU stub), `movie/movie.cmake`,
`include/dq8/movie/**` (with the coordinator), `cmake/Movie.cmake`, `src/runtime/platform/dynlib.cpp`,
`src/game/movies.*`, `src/host/movie_wiring.cpp`, the movie lines of `unwired_m4_option`;
`tests/unit/m4_movie_tests.cmake` and its tests.

**Delivers:**
- The HLE boundary: replace GetPicture `0x001113C8`, IsEnd `0x001114E8` and the video stream callback `0x001A06C0`
  (a host elementary-stream sink with backpressure at four pending pictures); stop on the inner IPU decode
  `0x00110B28`; hook Create, Delete (`0x00111380`, to confirm), AddCallback, AddStrCallback, DemuxPssRing, Reset and
  the player's open. GetPicture calls the player's nodata callback through `OverrideCall::call_guest`.
- `MpegEsModel` (picture split, reorder; the only judge of picture count and timing), the IPU colour conversion (from
  PCSX2's reference), the column-major macroblock writer, `SceMpegHle` (sessions keyed by mp; SDK knowledge only).
- The `dq8-movie` library: libavcodec + libavutil, `mpeg2video`, one thread, bit-exact flags, simple IDCT, SHOW_ALL;
  exports only `dq8_mvd_*`. `DQ8_FFMPEG=auto|system|off` (MSVC off); CI Linux jobs install
  `libavcodec-dev libavutil-dev`; a bundled LGPL build and Windows DLLs are M5.
- Policies: `auto` plays when the library loads (else skips with a `movie_decoder` event); `play` without it exits 3
  at start-up; `nodecode` runs the HLE with black pictures; with `play`/`nodecode` the flyover timeline write is off.

**First task:** confirm that the nodata callback yields, the Delete address, DemuxPssRing's behaviour on a refusing
callback, the open entry's name argument, and that `.MVI` data arrives by fileio path (if by CD streaming, that is
`cdvdfsv.cpp`, D's file: ask the coordinator).

**Done when:** streams from our own MPEG-2 writer (generated at test time, no fixture) round-trip the ES model, the
colour conversion, the writer and the library; the HLE drives a fake player loop; `nodecode` and a fake decoder give
identical events; the exports check passes.

### D: Saves, pad, VU0 follow-ups, kernel and overlay robustness (about 3,500 lines)

**Owns:** `iop/services/{mcserv,padman,cdvdfsv}.cpp`, `io/memcard/**`, `io/memcard.cmake`, `io/pad_feedback.*`,
`machine/rtc.{h,cpp}`, `core/platform/paths.*`, `platform/sdl/{sdl_platform,sdl_mapping}.*`, `vu/**`,
`dma/vif.cpp`, `ee/ee_runtime.cpp`, `ee/context.*`, `kernel/{scheduler,kernel_objects,services_system}.cpp`,
`host/{save,feedback}_wiring.cpp`, the card/RTC/VU0 lines of `unwired_m4_option`; `tests/unit/m4_save_tests.cmake` and
its tests; `Docs/formats/memory-card-folder.md`.

**Delivers (in priority order if time runs short):**
1. **The memory card:** an in-memory card model (authoritative for the guest) with folder persistence on the I/O
   thread and an `IMemoryCardStore` per port; mcserv translates GetInfo (−1/−2 on the first call, then 0; the format
   flag), Open, Close, Flush, Seek, Read, Write (inline head bytes plus the EE body), Chdir, GetDir (patterns, 0x40-byte
   entries), SetInfo, Delete, Format and Unformat; functions not yet seen stop. Folder layout
   `<save-dir>/<profile>/card1/` with `card.toml`, per-directory metadata, raw files, `.backup/` (three deep) and
   `.tmp/`; atomic replace; entry order from metadata; host mtimes ignored. The virtual RTC (`--rtc`), and cdvdfsv's
   ReadClock only if the game's libcdvd calls it. `--save-dir` (window: the user data directory; headless: in-memory),
   `--memory-card`, `--memory-card2`; the `card_sha1` end field.
2. **VU0 follow-ups:** interrupt handlers get a copy of the interrupted thread's VU0 block; a probe for cross-thread
   VU0 use; `--vu0-registers shared|per-thread`; VIF0 MSCAL on the same register view as VCALLMS.
3. **Pad:** every command the game's libpad can send; pressure mode only if asked; actuator bytes → `PadActuators` →
   `IPadFeedbackSink` → `IPlatform::set_rumble` → `SDL_RumbleGamepad`; `pad_rumble` events; `--rumble off`.
4. **Kernel:** SetMemoryMode as a documented noop; ResetEE accepted when masked units are idle (else stop); COP0
   stops decode the instruction (break 7 and 6, trap) and name the unit and function, flagging data decoded as code.
5. **Overlays:** the stale-return check in every build (overhead recorded); miss messages that name jump-table targets
   outside their function. The function-map audit itself is the coordinator's (bring-up step B1).

**First task:** Ghidra on the game's libmc (function numbers, layouts, results it distinguishes), libpad (commands,
actuator layout) and libcdvd (ReadClock?); a PCSX2 run with a formatted folder card for the save file names, sizes and
the fresh card's free clusters (names and sizes only go into the docs).

**Done when:** card model, folder and mcserv tests (format → write → read → getdir → delete; an interrupted write
keeps the old content; backups), RTC, pad commands and rumble events, VU0 inheritance and the switch, kernel and stop
message tests pass under ASan.

## Merge order and bring-up

**Merge:** D → C → A → B. All four start together. Bring-up runs from day 0 alongside the sessions.

| Step | When | Path | Settles |
|---|---|---|---|
| B0 | day 0 | M3 acceptance with the new defaults | Same hashes; done 2026-10-21 (title at vblank 1200, `af2abbd8…`, `bb194ddc…`) |
| B1 | day 0 → | New Game → field (`mark field`), movies skipped, null sound | The field renders (see the bring-up log), BATTLE's first execution, the function-map audit (`ExportFunctionMap.java`, `export_maps.sh`, re-export, re-recompile), new kernel calls; field speed |
| B2 | day 0 → | menu (Circle) open and close | MENU and the overlay round trip; stale returns |
| B3 | day 0 → | camp → road (the map: Square; follow the stream) → town → shop purchase | The route; SHOP; the gold word |
| B4 | day 0 → | leave town → encounter → battle won | The battle marker; VU1/VU0 in battle; a PCSX2 battle capture from our route |
| B5 | after D | church save (format first on the unformatted card) | The mcserv sequence, file names and sizes against a PCSX2 folder card, `card_sha1` |
| B6 | after D | a new process, the same `--save-dir`: Continue → field | Reload proven by RAM words |
| B7 | after C | logos, the TAKA_W flyover (750 pictures, silent), the opening P01 | The ES model on real streams; play against nodecode |
| B8 | after A+B | sound rows → `interpret` one by one: LIBSD → SDRDRV → MOD*/SK* → PCMPLAY; rerun B1–B7 | HLE gaps; SPU2 registers and RAM regions against PCSX2's `SPU2.bin`; IOP-to-EE RPC; AutoDMA; WAV parity |

**Scripts.** Record by hand (`--window --record-input`), rebase to trigger-relative lines with marks
(`tools/bringup/rebase_dq8in.py`), replay twice, and commit to `tests/local/input/m4/` with expectation files
(`tests/local/expect/*.toml`: relations, never game values) run by `tools/bringup/playthrough.py` as local CTests.
Later segments start from saves regenerated into `work/bringup/cards/`; saves are never committed. Audio tools:
`pcsx2_state.py` (IOP RAM and SPU2 from `.p2s`), `spu2_compare.py`, `wav_compare.py`. `tesseract` reads on-screen
text from scratch frames. Every finding goes into the bring-up log below.

## Acceptance

**Functional (local, real disc, headless, `--gs trace` and `parallel`):**
1. Segments from boot: New Game → field; menu open and close (MENU, then BATTLE); a town shop purchase (SHOP; gold
   decreases); a battle entered and won (experience increases); a church save (format, mkdir, writes; names and sizes
   equal PCSX2's folder card); a fresh process Continues into the field with the saved RAM words. No stops, zero
   dispatch misses, zero unimplemented services or HLE imports, no unhandled MMIO on the EE or IOP side.
2. Audio: the ten sound modules run interpreted; title, field and battle music, effects and voices play; SPU2
   registers and driver-written RAM regions equal PCSX2's `SPU2.bin` at the title, field and battle; WAV envelope
   correlation and loudness within the PCSX2-against-PCSX2 noise floor; ten minutes live with underruns recorded.
3. Movies: the logos, the flyover and the opening play with `play`; best-aligned frames reach SSIM ≥ 0.95 against
   PCSX2's software renderer; PCMPLAY consumes duration × 48 kHz ± 1 %; without the library `auto` skips and `play`
   exits 3.
4. Pad: the analog sticks drive the field; rumble events where the game drives the actuators; `window_smoke.py` (a
   uinput pad with FF_RUMBLE) sees SDL upload an effect and plays the field live.
5. Parity: menu, shop and battle UI crops SSIM ≥ 0.98 and full frames ≥ 0.95 at checkpoints (3D full frames
   recorded); the battle's GS privileged registers equal PCSX2's.

**Determinism:** per segment, two runs give identical events, GS hashes, RAM, IOP RAM, SPU2 RAM, audio hash and
`card_sha1`; `--speed 1` equals `max`; the null, trace and parallel backends agree on RAM; the null, SDL and WAV audio
sinks agree; `play` equals `nodecode` apart from GS hashes and the decode slots; Debug equals Release at segment ends.

**Always:** CI green on every job (Linux builds and tests dq8-movie with the system libavcodec; MSVC with the GS and
movie libraries off); checker, pre-commit and `reuse lint` clean; the M2 stop and the M3 boot tests pass.
**Recorded:** vblanks per second per segment (Release and Debug, null and parallel), IOP MIPS, SPU2 ms per field,
decode ms per picture, VU0 cycles, save latency.

## Protocol notes

*(Filled in by the sessions: the HLE IOP kernel structures the modules read; IOP-as-client RPC packets; the sceMpeg
HLE; mcserv requests and replies; padman commands and the actuator layout. Offsets and ids only, re-derived; no
ps2sdk text.)*

### The interpreted IOP (session A, 2026-10-22)

Re-derived from the formats and the IOP kernel's observable behaviour; items marked *inferred* are choices to
confirm in bring-up B8. Code: `src/runtime/iop/{cpu,hw,loader,kernel}/`, `iop_exec.*`.

**IRX image.** ELF type 0xFF80; the program header of type 0x70000080 points at the .iopmod record: +0x00 module
info pointer, +0x04 entry, +0x08 gp, +0x0C text size, +0x10 data size, +0x14 bss size, +0x18 version (16 bits),
+0x1A name (NUL-terminated). One PT_LOAD at address 0. REL types 2 (32), 4 (26), 5/6 (HI16/LO16, every HI16 paired
with the next LO16) against the image base, symbol index 0; any other type, a RELA section or a symbol stops the
load. Images go to sysmem's lowest free block (256-byte units).

**Import and export tables.** Import (in the text, found by scanning it): +0x00 0x41E00000, +0x04 link (0 in the
file), +0x08 version (16 bits; the high byte is the major), +0x0A flags, +0x0C name (8 bytes), +0x14 stubs of two
words (`jr $ra`, `addiu $zero, $zero, ordinal`) ended by two zero words; a word equal to the magic without a
printable name and a well-formed first stub is not a table. Linking writes `j target` over a stub's first word.
Export (registered at start): +0x00 0x41C00000, the same header, function addresses from +0x14 (ordinal n at
+0x14 + 4n) ended by 0. HLE libraries win over guest ones; a guest library may not take an HLE name (-212).

**HLE page.** Physical 0x0FF00000 (above the RAM mirrors, in reach of `j`), 64 KiB of 8-byte slots: the trap
word 0x6C000000 | slot (primary opcode 0x1B) and a zero word, as loads read them; fetching a slot is the trap.
Slot 0 thread exit, 1 module start return, 2 RPC function return, 3 interrupt return, then the 66 HLE functions in
library and ordinal order, then one stop trap per unimplemented (library, ordinal) imported.

**Module start.** A thread at priority 8 (*inferred*: start returns before the threads it starts unless they
block it) with a 16 KiB stack runs start(argc, argv): argv[0] is the load path, the NUL-separated loadfile
arguments follow; strings and vector sit at the top of that stack. Result 0 resident, 1 not resident (the image is
freed), 2 removable. The loadfile reply (id, start result) leaves once start returns.

**Threads.** CreateThread parameter: +0x00 attr, +0x04 option, +0x08 entry, +0x0C stack size, +0x10 priority
(1-126; lower runs first; no time slicing). Stacks come from the top of the heap (sysmem mode 1) and are filled with
0xFF unless attr has 0x00100000. A started thread has $sp = stack top - 16, $gp = its creator's, $a0 = StartThread's
argument; returning from its entry makes it dormant. ReferThreadStatus writes 13 words: attr, option, status (1 run,
2 ready, 4 wait, 0x10 dormant), entry, stack, stack size, gp, initial priority, current priority, wait type (1
sleep, 2 delay, 3 semaphore, 4 event flag; RPC waits report 1), wait id, wakeup count, 0. Thread 0 in a call means
the caller.

**Semaphores and event flags.** CreateSema parameter: +0x00 attr (bit 0: waiters by priority, else FIFO), +0x04
option, +0x08 initial, +0x0C max. CreateEventFlag: +0x00 attr (bit 1: several waiters allowed), +0x04 option, +0x08
initial bits. WaitEventFlag mode: bit 0 OR (else AND), 0x10 clear the bits waited for, 0x20 clear all; the result
word gets the bits before the clear. ClearEventFlag ANDs. Deleting either wakes its waiters with -425.

**Time.** The system clock counts IOP cycles (36.864 MHz) since the IOP's last boot; GetSystemTime and
USec2SysClock write (low, high) words; USec2SysClock = µs × 36,864 / 1,000; SysClock2USec splits into seconds and
microseconds. DelayThread(µs) is an "iop_delay" event; iSetAlarm(clock, handler, common) an "iop_alarm" event whose
handler returns the next interval in ticks (0 ends it); CancelAlarm/iCancelAlarm take (handler, common) and return
-105 when none matches (*inferred*).

**Interrupts.** Numbers 0-31 are INTC lines; 0x20 + n is DMA channel n (0-6) and 0x28 + (n - 7) channels 7-12,
demultiplexed from line 3 by DICR (channel enables in bits 16+, flags in bits 24+, master enable bit 23) and DICR2
(channels 7-12, same bit layout). EnableIntr on a DMA number sets the channel's enable, the master enable and I_MASK
bit 3. Handlers run one at a time on an 8 KiB interrupt stack as handler(arg) with the registering code's $gp, when
the running thread has interrupts enabled (CpuSuspendIntr writes 1 or 0 to its state word and returns -102 when
already suspended); one that returns 0 has its interrupt disabled (*inferred*). QueryIntrContext is 1 in a handler.
Blocking calls return -100 there and -417 with interrupts suspended.

**Hardware.** I_STAT 0x1F801070 (a write ANDs), I_MASK 0x1F801074, I_CTRL 0x1F801078 (a read returns and clears
it); DMA channel n at 0x1F801080 + 0x10n (0-6) or 0x1F801500 + 0x10(n - 7): MADR, BCR, CHCR (bit 24 busy), TADR;
DPCR 0x1F8010F0 and DPCR2 0x1F801570 (enable = bit 4k + 3), DICR 0x1F8010F4, DICR2 0x1F801574 (flags write-1-to-clear),
DMACEN 0x1F801578, DMACINTEN 0x1F80157C; SSBUS stores at 0x1F801000-0x1F801020 and 0x1F801400-0x1F801420; the
scratchpad at 0x1F800000 (1 KiB). Only whole-word stores reach I_STAT, I_CTRL, DICR, DICR2 and CHCR. 32-bit
accesses to a mapped device arrive as two 16-bit accesses, low half first. Starting a channel no device maps, or
one DPCR/DPCR2 leaves disabled, stops; so does any other device-space address.

**timrman.** Timer ids are (index + 1) << 28 | (register address >> 4); allocation order 2, 5, 4, 3, 0, 1; source
masks 0x0B, 0x0D, 0x01, 0x05, 0x01, 0x01; widths 16, 16, 16, 32, 32, 32 bits; maximum prescale 1, 1, 8, 1, 256, 256
(table adapted from PS2Recomp). Sources: 1 system clock, 2 pixel clock (13.5 MHz), 4 horizontal blank. A started
timer with a handler raises "iop_timer" after `compare` counts; the handler(common) returns the next compare (0
stops the timer); the counter restarts at each compare. SetupHardTimer modes 0, 1, 3, 5, 7.

**sysmem.** AllocSysMemory(mode, size, address): 0 lowest fit, 1 highest fit, 2 the block holding `address`, in
256-byte units, 0 when it does not fit; other modes stop. FreeSysMemory returns -1 for an address that starts no
block (*inferred*). Kprintf and printf use our formatter and go to the `iop` trace category line by line.

**IOP RPC structures.** Server structure (0x44 bytes, the handle the EE binds to): +0x00 sid, +0x04 function,
+0x08 buffer, +0x0C argument size, +0x10 cfunction, +0x14 cbuffer, +0x24 function number, +0x2C reply size, +0x40
queue. Queue (0x18 bytes): +0x00 the serving thread. The function runs as function(fno, buffer, size) on the
sceSifRpcLoop thread and returns the reply's address; reply size bytes are copied from it. Client structure (0x28
bytes): +0x00 packet address (the structure itself), +0x04 request number, +0x08 thread, +0x0C mode (bit 0 no
wait), +0x10 command, +0x14 server buffer, +0x18 cbuffer, +0x1C end function, +0x20 end parameter, +0x24 server
(0 until a bind ends with one).

**IOP-to-EE RPC packets.** Bind (0x24 bytes): header size 0x24, cid 0x80000009; +0x10 0, +0x14 client, +0x18 request
number, +0x1C client, +0x20 sid. Call (0x38 bytes): header word 0 = 0x38 | send size << 8, word 1 = the server's EE
buffer, cid 0x8000000A; +0x10 0, +0x14 client, +0x18 request number, +0x1C client, +0x20 fno, +0x24 send size, +0x28
the IOP receive address, +0x2C receive size, +0x30 1 (an end packet is always asked for), +0x34 server; the send
data is written to the EE buffer ahead of the packet. The EE's end packet (cid 0x80000008) echoes +0x10..+0x1C and
gives +0x20 the ended command, +0x24 server, +0x28 buffer, +0x2C cbuffer; a call's reply data reaches the receive
address before it. The end function runs as end(param) in the interrupt context; a waiting caller wakes with 0.
`Iop::on_command` routes every cid 0x80000008 to the interpreted IOP (the native services never make requests).

**sifman.** sceSifSetDma(descriptors, count): 16-byte descriptors (IOP source, EE destination, size, attribute);
attribute 0 or 0x40 only (interrupt bits stop); sizes round up to words; a data-only SIF0 delivery. sceSifDmaStat:
1 while queued, -1 once delivered (the last 256 ids are remembered).

**Error codes returned.** -100 illegal context, -101 illegal interrupt number, -102 interrupts already suspended,
-103 interrupt already disabled, -104/-105 handler found/not found, -150 to -156 timers (no timer, bad id, source,
prescale, busy, not set up, not in use), -212/-213/-214 library found/not found/illegal, -400 no memory, -402
entry, -403 priority, -404 stack size, -405 mode, -406 thread id, -407 unknown thread, -408 unknown semaphore, -409
unknown event flag, -413 dormant, -414 not dormant, -417 cannot wait, -420 semaphore overflow, -422 event flag
multiple waiters, -423 empty pattern, -425 deleted while waiting.

**Events.** `iop_module` (native rows: mode "native", base, size and start 0), `iop_link`, `iop_import_unimplemented`,
`iop_rpc_register`; with `--dump-iop-ram-at`, `iop_ram_dump {sha1}`. Event tags: "iop_slice", "iop_delay",
"iop_alarm", "iop_timer", "iop_dma".

## Bring-up log

| Unknown | Local evidence | Resolution (addresses, ids, offsets, chosen values) | Change |
|---|---|---|---|
| Where New Game goes (2026-10-21) | Probe: title menu → Create → name entry → save prompt "No" with scripted input; events; PCSX2 `field.p2s` and `menu.p2s` | Mode 0 at vblank 1688 and BATTLE (id 5) resident at 1699 through the hook; the field keeps BATTLE, the main menu swaps MENU (id 4) in | None: the residency hook handles both |
| The first VU0 micro program | Stop at vblank 1786: VCALLMS at `0x0013CC7C` (`FUN_0013CC70`); the other site is `FUN_0013CCE0`; both start at 0 | VU0 micro mode from EE code was unsupported | Day-0 VU0 bridge (`vu0_bridge.*`, `VuUnits::run_from_ee`) |
| VU0 time growing with every call | With the bridge: about 9,500 programs by vblank 2300 at a mean of 125,000 VU cycles, rising; one frame every 6–7 vsyncs | The interpreter's cycle count runs on across programs; the copy-in zeroed the visible field and the bridge charged the running total each call (16, 32, 48, … for a 16-cycle program) | The copy-in keeps the count and the bridge returns the difference (unit test "a second run … costs the same"); now about 25 cycles per program and 30 Hz again |
| The field draws no 3D (open, B1) | After the skipped opening movie: a blue screen with Yangus's dialogue on top at 30 Hz. `machine.vu`: VU1 runs 0 programs. GS: about 1,250 sprite kicks per frame, no triangles, no PATH1. VIF1: about 158,000 quadwords per frame (texture uploads through DIRECT, 2D sprites), 254,000 call/ret sub-chains with no MSCAL in their tag words, no MSCAL/MSCNT VIFcodes at all. The disc loads the field at vblanks 1600–1900 | MSCAL (`0x1400xxxx`) and MSCNT (`0x1700xxxx`) VIFcodes are built only in `FUN_00141EA0` (reached once, vblank 1992, as a `ret` tag with MSCAL 0x64), `FUN_0028C870` and `FUN_00149DD0` (never reached). The game builds no 3D draw list. Leading hypothesis: VU0 results (visibility or culling) differ, so every object is culled; to check against PCSX2 VU0 register state | Open: bring-up step B1 |

## Results

*(Filled in at acceptance: counts, vblanks, SSIM, timings; addresses only.)*
