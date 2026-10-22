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

### The sceMpeg HLE (session C, 2026-10-22)

Static evidence from the boot ELF (headless Ghidra on a copy of `work/ghidra/base`, outputs in `work/ghidra/m4c-*`,
git-ignored). Every address is in the main unit.

**What the player registers.** The open routine `0x001A1C60` creates the decoder on the player's `mp` (`0x00410A60`)
and registers five general callbacks with AddCallback (type, function, data 0): 0 error `0x001A0B30`, 1 nodata
`0x001A0B00`, 2 stop-DMA `0x001A0AD0`, 3 restart-DMA `0x001A0AA0`, 5 timestamp `0x001A0A50`. AddStrCallback then
registers stream type 0 (video), channel 0, `0x001A06C0`, and, when the movie has audio, type 2 (PCM), channel 0,
`0x001A0470`; the data word of both is the demux ring's base. The HLE takes the nodata callback from the AddCallback
hook (type 1), never from this table.

**The open entry's arguments.** The player entry `0x001A2450` (context, request block) calls the open with $a0 the
work area, $a1 the request block, **$a2 the movie name** (a NUL-terminated string at request + 0x80, the name before
`0x0019F2F0` turns it into the `cdrom0:` path with the `.MVI` extension), $a3 a table of allocation handles, $t0 and
$t1 the picture width and height in pixels (request + 0xD8 and + 0xDC), $t2 the audio flag byte and $t3 a second
flag byte. The decoder thread asks GetPicture for (width / 16) × (height / 16) macroblocks.

**The nodata callback yields.** `0x001A0B00` calls RotateThreadReadyQueue at the priority in the word at
`0x003D200C` (`gp - 0x7764`), the priority all three player threads are created with (helper `0x001A1050`, decoder
`0x001A0FE0`, reader `0x001A0B40`), then the staging-ring refill `0x0019FD80`, and returns 1. The refill waits on the
ring's semaphore and, while the flag at ring + 0x44 is set (the open and `0x001A0150` set it, the stop-DMA callback
clears it), stops DMA channel 4 (CHCR written without STR), reads its CHCR and MADR, turns the whole 2 KiB slots the
video callback has written since into REF tags, and starts channel 4 again only when a slot is queued. Under the HLE
the video callback is replaced, so no slot is ever queued: the callback yields, writes CHCR without STR, reads two
registers, and never starts the channel. Calling it through `OverrideCall::call_guest` is therefore safe; the yield is
what lets the reader thread demux more data.

**Delete.** `0x00111380` is sceMpegDelete: 8 bytes that return 1. Its only caller is the stop routine `0x001A13D0`,
with the player's `mp`, after the three threads are terminated, channel 4 is stopped and cleared, and the ring's
semaphore is deleted.

**DemuxPssRing and a refusing callback.** `0x001155C0` takes (mp, start, bytes, ring base, ring size). It parses
pack headers and PES packets from `start`, wrapping at the ring's end, and calls the stream callback whose (id, mask)
row matches, with a block on its stack: +0x00 type (6, a stream), +0x04 the PES header, +0x08 the payload, +0x0C the
payload length, +0x10 PTS and +0x18 DTS (64-bit, negative when absent). The payload pointer is wrapped into the ring,
but the payload itself may run past the ring's end and continue at its base. **A callback that returns 0 stops the
demux at once**; the return value is the byte count up to the end of the last accepted packet (a packet no callback
matches counts as accepted). The reader `0x001A0B40` subtracts that count from the ring's fill word, so the refused
packet stays in the ring and is offered first on the next call. A refusing video callback is therefore a working
backpressure signal; the reader yields between calls.

**How `.MVI` data arrives.** Neither fileio nor CD streaming: the read routine `0x0019F130` reads through the game's
file layer (request slot `0x0024E6E0`, data ring `0x0024FEB0`), whose I/O thread (`0x0024F5E0` → `0x0024E990`) picks a
device kind from the path's prefix (`0x00162980`, a five-entry name table at `0x00395030`). The `cdrom0:` prefix is
kind 4: sceCdSearchFile (`0x0012A970`, via `0x00161ED0`) and then sceCdRead by LSN (`0x0012B5B8`, via `0x001611A0` →
`0x00161880`), the same libcdvd N-command path M3's native cdvdfsv serves for the game's other files. No sceCdSt* call
is involved; nothing in cdvdfsv needs to change for movies. (Kind 0, the host device, would use fileio.)

**The decoder loop and `mp`.** `0x0019E840` loops while IsEnd returns 0: it waits (RotateThreadReadyQueue) for a free
decode slot, calls GetPicture (mp, the slot's uncached address, the macroblock count), prints a message when the
result is negative, and builds the slot's GIF packets when `mp + 8` reads 0 after the call (the first picture). After
the loop it calls Reset. The sceMpeg structure the HLE fills: +0x00 width, +0x04 height, +0x08 the index of the
picture just delivered (0 for the first), +0x10 PTS and +0x18 DTS (64-bit), +0x40 the SDK's private work area (the
library's own; the HLE does not read it). GetPicture's outer half `0x001113C8` stores the destination, the count and
two zero words in that work area and calls the inner decode `0x00110B28`, which the HLE never reaches (it stops).

**The stop routine.** `0x001A13D0` first calls `0x001A1170`, which appends a four-byte code to the IPU staging ring and
bumps the ring's byte counters (harmless under the HLE: no DMA), sets the reader's stop flag and waits for the reader
to return, then terminates and deletes the three threads. The decoder thread may be inside GetPicture, and so inside
the nodata callback: the HLE keeps no host state across that call.

**The end of a movie.** The manager loop ends when the reader's remaining-bytes word drops below 5 or the word at
mp + 0xA8 (`0x00410B08`) reads 3, which the decoder thread writes after its loop (IsEnd true, then Reset). sceMpegCreate
calls Reset itself, so the Reset hook also runs once inside Create.

**What the HLE does (session C, `runtime/movie/sce_mpeg_hle.*`).**
- Sessions are keyed by `mp`, created by the Create hook (which takes the name the player-open hook recorded) and
  dropped by the Delete hook. The AddCallback hook keeps type 1 (nodata); the AddStrCallback hook stops the machine
  if stream type 0 is registered to anything but the profile's `video_stream_callback`; the DemuxPssRing hook
  records the ring's base and size ($a3, $t0) so the video callback can follow a payload across the ring's end.
- The video callback replacement copies the payload into `MpegEsModel` and returns 1, or returns 0 without reading
  while four complete pictures are not yet delivered. GetPicture delivers the next picture in display order: the
  decoder is fed coded pictures tagged with their decode-order index until it returns that tag (black, and counted,
  if it cannot); the IPU CSC with SETTH's thresholds turns it into RGB32 macroblocks (column-major, 1 KiB each, as many
  as $a2 asks); mp + 0/4/8 and the PTS and DTS at + 0x10 and + 0x18 are written; `movie_picture` cycles are charged.
  With no picture ready it calls the nodata callback with a data block whose type word is 1, as often as needed; a
  callback result of 0 makes GetPicture return -1. IsEnd is 1 once the stream's sequence end code has been seen and
  every picture delivered.
- Events: `movie {phase, name, pictures, slot_a, slot_b}` with phase `open` (the player-open hook), `end` (IsEnd first
  1) and `delete`; `slot_a` and `slot_b` are the physical addresses of the first two GetPicture destinations (each
  holds macroblocks × 1 KiB), the RAM a play-against-nodecode comparison masks. `movie_decoder {available: false,
  reason}` once at start-up when `--movies auto` falls back to skip.

## Bring-up log

| Unknown | Local evidence | Resolution (addresses, ids, offsets, chosen values) | Change |
|---|---|---|---|
| Where New Game goes (2026-10-21) | Probe: title menu → Create → name entry → save prompt "No" with scripted input; events; PCSX2 `field.p2s` and `menu.p2s` | Mode 0 at vblank 1688 and BATTLE (id 5) resident at 1699 through the hook; the field keeps BATTLE, the main menu swaps MENU (id 4) in | None: the residency hook handles both |
| The first VU0 micro program | Stop at vblank 1786: VCALLMS at `0x0013CC7C` (`FUN_0013CC70`); the other site is `FUN_0013CCE0`; both start at 0 | VU0 micro mode from EE code was unsupported | Day-0 VU0 bridge (`vu0_bridge.*`, `VuUnits::run_from_ee`) |
| VU0 time growing with every call | With the bridge: about 9,500 programs by vblank 2300 at a mean of 125,000 VU cycles, rising; one frame every 6–7 vsyncs | The interpreter's cycle count runs on across programs; the copy-in zeroed the visible field and the bridge charged the running total each call (16, 32, 48, … for a 16-cycle program) | The copy-in keeps the count and the bridge returns the difference (unit test "a second run … costs the same"); now about 25 cycles per program and 30 Hz again |
| The field draws no 3D (open, B1) | After the skipped opening movie: a blue screen with Yangus's dialogue on top at 30 Hz. `machine.vu`: VU1 runs 0 programs. GS: about 1,250 sprite kicks per frame, no triangles, no PATH1. VIF1: about 158,000 quadwords per frame (texture uploads through DIRECT, 2D sprites), 254,000 call/ret sub-chains with no MSCAL in their tag words, no MSCAL/MSCNT VIFcodes at all. The disc loads the field at vblanks 1600–1900 | MSCAL (`0x1400xxxx`) and MSCNT (`0x1700xxxx`) VIFcodes are built only in `FUN_00141EA0` (reached once, vblank 1992, as a `ret` tag with MSCAL 0x64), `FUN_0028C870` and `FUN_00149DD0` (never reached). The game builds no 3D draw list. Leading hypothesis: VU0 results (visibility or culling) differ, so every object is culled; to check against PCSX2 VU0 register state | Open: bring-up step B1 |
| Does the movie player's nodata callback yield? (session C, 2026-10-22) | Ghidra: `0x001A0B00`, its callee `0x0019FD80`, the thread set-up `0x001A1A00` | Yes: RotateThreadReadyQueue at the player threads' common priority, then a staging-ring refill that starts DMA channel 4 only when the video callback has queued a slot, which the HLE never does | GetPicture calls it through `call_guest` (Protocol notes) |
| Delete's address (session C) | Ghidra: `0x00111380` and its one caller | `0x00111380` is sceMpegDelete (returns 1; called once from the stop routine `0x001A13D0` with the player's mp) | `delete` in `[movie.sce_mpeg]` confirmed |
| DemuxPssRing with a refusing callback (session C) | Ghidra: `0x001155C0`, the reader `0x001A0B40` | Stops at the refused packet and returns the bytes up to the last accepted one; the reader keeps the rest in its ring, so the packet is offered again | The video callback replacement refuses at four pending pictures |
| The open's name argument (session C) | Ghidra: `0x001A2450`, `0x001A1C60`, `0x0019F2F0` | $a2 of the open, a C string (the name before the path and extension rewrite); $t0/$t1 width and height | The `movie` events carry it |
| How `.MVI` data arrives (session C) | Ghidra: `0x0019F130` → the file layer `0x0024E990` → `0x00162980`, `0x00161ED0`, `0x00161880` | The `cdrom0:` device: sceCdSearchFile, then sceCdRead by LSN through libcdvd's N-commands; not fileio, not CD streaming | None: M3's cdvdfsv already serves both |
| The flyover under the HLE (session C, 2026-10-22, real disc, Clang Debug, `boot-to-title-settings-only.dq8in`, `--gs trace`) | `--movies nodecode` and `play`; override call counts in `--json`; a temporary stderr log of the HLE (not committed) | The player opens at vblank 1143 (threads 11–13, priority 40): Create, five AddCallback, two AddStrCallback (video and PCM). The video arrives as expected (the first payload starts with the sequence header, 512 × 448, not low-delay; PES PTS every few packets), backpressure refuses while four pictures wait, and pictures are delivered every second vblank from 1146 in I B B P order; with `play` dq8-movie decodes them with 0 errors and 0 tag mismatches (the first eleven pictures are black). At vblank 1162 the video stops arriving: the PCM stream callback `0x001A0470` is called (the TAKA flyover carries the PCM stream), and with the sound modules as null servers the game's PCM ring never drains, so DemuxPssRing stops in front of it and the decoder waits in GetPicture's nodata loop for ever. Accepting and dropping the PCM packets instead (a local experiment) stalls earlier: the reader starts playback only when the audio preload threshold is reached | Open, B7 after A+B: movies need PCMPLAY's play position to advance (session A's interpreted PCMPLAY and session B's SPU2). Until then `--movies auto` with dq8-movie built (or `play`, `nodecode`) stops the title sequence at the flyover; use `--movies skip` or `blank` |

## Results

*(Filled in at acceptance: counts, vblanks, SSIM, timings; addresses only.)*
