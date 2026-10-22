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
normalisation, wildcards, attributes). PCSX2's SPU2 behaviour may be adapted (GPL-3.0-or-later, attributed); session B adapted
`pcsx2/SPU2/` at v2.7.524 (`3e29183a37e74cbc8c17bda8afb63c2d9bc6fd14`, git-ignored clone in `work/ref/pcsx2`). The dev
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
| `spu2_dma_per_halfword` | 192 (24 IOP cycles) | manual SPU2 DMA time per 16-bit word: PCSX2's `CYCLES_PER_WORD` (`pcsx2/SPU2/Dma.cpp`), with its chunking (a write moves `min(R, 256 + R/24)` words at each chunk's start, a read 256 at each chunk's end plus one word's time) |
| `movie_picture` | 1,179,648 (4 ms) | one picture the movie HLE delivers |
| `mc_latency_us`, `mc_bytes_per_second` | 200, 1,000,000 | memory-card replies |

**Events added in M4** (deterministic fields only): `iop_module` gains `mode`, `base`, `size`, `start`;
`iop_link {module, hle, guest, unimplemented}`, `iop_import_unimplemented {module, library, ordinal}`,
`iop_rpc_register {sid, module}`, `audio {frames, hash}` (only when frames changed), `movie {phase, name, pictures,
slot_a, slot_b}`, `movie_decoder {available, reason}`, `mc_op {op, port, path, result}`, `pad_rumble {port, small,
large}`, `mark {name}`, `spu2_dump {sha1}` (`--dump-spu2-at`). The `end` event gains fields through `M4Wiring::end_fields` (`audio_hash`, `card_sha1`).

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
| B0 | day 0 | M3 acceptance with the new defaults | Same hashes; done 2026-10-21 (title at vblank 1200, `af2abbd8…`, `bb194ddc…`). Since B1's VF0 fix (2026-10-22) the RAM SHA-1 at the title is `a524a87d…`, and since session D's merge (the card answers GetInfo -2 once, then 0) `ff521066…`; the GS stream is unchanged. Since B8 (2026-10-22, the sound modules interpreted) the title comes at vblank 1207 with RAM `d091b0d3…` and GS stream `7cef83ae…` |
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

### Session D: memory card, pad, CD clock (2026-10-22)

Re-derived from the game's EE-side libmc, libpad and libcdvd and its save code in the local Ghidra project (M3
session C's export plus new queries; raw output in `work/m3c/` and `work/m4d/`), and from the game's strings. Words
are little-endian, "+N" a byte offset. M3-boots.md "Protocol notes" has the RPC framing and the M3 layouts; this
adds what saving needs. Code: `src/runtime/iop/services/mcserv.cpp`, `padman.cpp`, `src/runtime/io/memcard/`.

**libmc (server 0x80000400, client at `0x003DDC80`).**
- Function numbers: 0xFE init (blocking, 12-byte reply: 0, mcserv version ≥ 0x20A, mcman version ≥ 0x20E), 1
  GetInfo, 2 Open (also Mkdir: flags 0x40), 3 Close, 4 Seek, 5 Read, 6 Write, 0x0A Flush, 0x0C ChDir, 0x0D GetDir,
  0x0E SetInfo (also Rename: flags 0x10), 0x0F Delete, 0x10 Format, 0x11 Unformat. The library also has wrappers
  for 0x12 (GetEntSpace), 0x15 and 0x35 that nothing calls, and ChDir's wrapper is unreferenced; our server
  translates ChDir anyway and stops on 0x12, 0x15, 0x35 and every other number.
- Every call but init is no-wait with one 4-byte result word; `sceMcSync` returns it with the library's command
  id (Mkdir 0x0B, Rename 0x13, otherwise the function number). The game checks the command id after Flush (0x0A),
  Close (3), GetDir (0x0D) and Unformat (0x11).
- Descriptor requests (0x30 bytes): +0 handle, +4 port, +8 slot, +0x0C size (GetInfo: "format wanted"), +0x10
  offset (GetInfo: "free wanted"), +0x14 origin, inline byte count (GetInfo: "type wanted"), +0x18 EE buffer,
  +0x1C EE parameter block, +0x20 16 inline bytes.
- Path requests (0x414 bytes): +0 port, +4 slot, +8 flags or mode, +0x0C maxent, +0x10 EE table or record, +0x14
  the path (0x400).
- GetInfo: the server writes the 0xC0-byte block at +0x1C: +0 type (2 = PS2 card), +4 free clusters, +0x90
  format (1 formatted, 0 not). Our results: the first GetInfo after insertion answers -1 (formatted) or -2
  (unformatted), every later one 0; no card answers -12 with type 0. After Format and after Unformat the game
  requires a result ≥ 0.
- Read: data straight into +0x18; the fix-up block at +0x1C gets zero head and tail counts (16 bytes). Write: the
  library sends `head = (16 - (buf & 15)) & 15` bytes inline at +0x20 with the count at +0x14, the rest (+0x0C
  bytes) from EE +0x18; a write of at most 16 bytes travels inline whole. The result is the bytes written.
- GetDir: up to maxent 0x40-byte records to +0x10: +0 created and +8 modified (each: +1 seconds, +2 minutes, +3
  hours, +4 day, +5 month, +6 year as u16), +0x10 size, +0x14 attributes (u16), +0x20 the name (32 bytes). The
  game reads only the size and the name. Mode +8 is always 0.
- SetInfo: +8 = flags & 7 (1 created, 2 modified, 4 attributes); Rename sends 0x10 with the new name at +0x20 of
  the 64-byte record whose EE address is at +0x10.
- Results the game distinguishes: ≥ 0 success (a handle, a count, a position); -1 and -2 (GetInfo, above); -4
  from Mkdir means "exists" and from Delete "already gone", both tolerated; < -10 means no card; -200 (the
  library's own "busy") makes it retry; any other negative result is a failure.
- Wildcards: the game finds its saves with `/BASLUS-21207dq8???` and expects `BASLUS-21207dq8_1` to match, so a
  `?` that finds the name already ended matches nothing (otherwise one character).

**The game's save files** (names from its strings and save context; sizes from its size table).
- One directory per adventure log, `/BASLUS-21207dq8_N` (five on screen). It holds `icon.sys` (964 bytes),
  `dq8ic.ico`, `dq8ic_c.ico` and `dq8ic_d.ico` (49,726 bytes each), and the data file `BASLUS-21207dq8_N` (23,776
  bytes). A log is valid when GetDir of `.../icon.sys` gives size 964 and the data file has at least 23,712
  bytes.
- Creating a log: Mkdir, `icon.sys` with Open 0x202, the three icons with Open 0x203, each written in 0xC00-byte
  chunks, flushed and closed. Saving over a log: Rename `icon.sys` to `icon.err` (the "write in progress"
  marker), the data file with Open 0x202 (23,776 bytes, flush), Seek 4 and a one-byte write, Flush, Close; Open
  `icon.err` with mode 3, rewrite its 964 bytes, Flush, Close, Rename back to `icon.sys`, GetInfo. Deleting:
  GetDir `dir/*` (maxent 16), Delete from the third record on (the first two are `.` and `..`), Delete the
  directory, GetInfo.
- PCSX2 with an empty folder card (only an empty `_pcsx2_superblock`): the game sees an unformatted card, lists
  five unused logs, and asks whether to format it now (No by default). The scripted run did not
  get past that prompt reliably on the loaded host (states in `work/pcsx2/probe-20261022-*`), so the files
  PCSX2 writes and a fresh card's free clusters are still to be compared in bring-up step B5. Our card reports
  8,000 free clusters when freshly formatted (*inferred*).

**libpad (servers 0x80000100 and 0x80000101).** Every call is function 1 with a 0x80-byte block (+0 command, +4
port, +8 slot); the commands the library can send and our replies:

| Command | Name (by behaviour) | Request | Reply |
|---|---|---|---|
| 0x01 | port open | +0x10 EE pad area | +0x0C 1; +0x14 IOP actuator buffer (0x40 bytes) |
| 0x06 | set main mode | +0x0C mode index, +0x10 lock | +0x14 1 |
| 0x08 | set actuator alignment | +0x0C..+0x11 six bytes | +0x14 1 |
| 0x0A | set button info (pressure) | +0x0C button mask | +0x10 1 |
| 0x0B | set reference parameters | +0x0C..+0x17 | +0x1C 1 |
| 0x0C | port count | – | +0x0C 2 |
| 0x0D | slot count | +0x04 port | +0x0C 1 (0 past the last port) |
| 0x0F | end | – | +0x0C 1; the ports close |
| 0x10 | init | – | +0x0C 1 |
| 0x12 | module version | – | +0x0C 0x0400 |
| 0x14 | not determined | +0x04 | stops (no caller) |
| 0x18 | the library's SIF-command callback on or off | +0x04 0 or 1 | nothing |

- The game itself sends 0x12, 0x10, 0x01, 0x06 (only for a digital pad) and 0x08 with the table {0, 1, 0xFF,
  0xFF, 0xFF, 0xFF}: direct byte 0 drives actuator 0 (the small motor, on or off), byte 1 actuator 1 (the large
  motor, 0..255).
- The motors take no command: SetActDirect sends a 32-byte block with SIF DMA into half `counter & 1` of the IOP
  actuator buffer: +0 a counter, +4 1, +8 6, +0x0C..+0x11 the six direct bytes. The half with the larger counter
  is the newest. Our server reads it at every vblank and maps it through the alignment table.
- Pressure: frames report id 0x79 and the twelve pressure bytes only after 0x0A with a non-zero mask; until then
  0x73 (analog) or 0x41 (digital).

**libcdvd.** The library has no ReadClock S-command (and no other clock or status S-command), so `cdvdfsv`
needs none; the memory card's timestamps come from our virtual RTC (`--rtc`).

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
- Events (agreed with the coordinator 2026-10-22). `movie {phase, name, pictures, slot_a, slot_b}`:
  - The phases are `open` (the player-open hook, before Create), `end` (the first IsEnd that returns 1) and `delete`
    (the Delete hook); `name` is the open's name argument (empty without `player_open` in the profile) and
    `pictures` the pictures delivered so far.
  - `slot_a` and `slot_b` are the physical addresses (hex) of the first two GetPicture destinations, the game's two
    decode slots (each holds macroblocks × 1 KiB): a play-against-nodecode RAM comparison masks them, and the
    event logs of the two runs stay identical.
  - `movie_decoder {available: false, reason}` is emitted once at start-up, from the main thread before the machine
    thread starts, and only when `--movies auto` falls back to skip; `play`, `nodecode` and an `auto` that plays emit
    none.
- The `--movies auto` probe (`game::resolve_movie_policy`, `rt::movie_library_problem`) loads dq8-movie and checks its
  ABI version and functions, nothing more: no decoder, no guest state. A library it refuses is unloaded. One it
  accepts stays loaded, as the HLE's own loading does: FFmpeg's dependencies (libgomp through libavcodec,
  libswresample and libsoxr) allocate when loaded, and an unload makes LeakSanitizer fail every ASan run.
- The player-open hook is installed when `[movie.sce_mpeg].player_open` is set. On SLUS-21207 the open `0x001A1C60` is
  entered by a tail jump from the player entry `0x001A2450`, which the dispatcher cannot intercept (a hook there ran 0
  times), so the key names the open's first call, `0x0019E430` (its only caller is the open), whose `$a0` is the name.

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
| The field draws no 3D (B1, resolved 2026-10-22) | After the skipped opening movie: a blue screen with dialogue on top at 30 Hz; VU1 ran no program. A VIFcode histogram to vblank 2600 (temporary instrumentation): VIF1 uploads one 88-doubleword VU1 program at micro address 0 every frame (1,006 MPGs) and about 15 V4-32 UNPACKs per frame, but no MSCAL, MSCALF or MSCNT. One frame's VIF1 chain has 1,110 tags (224 `call`, 224 `ret`, 425 `cnt`, 236 `ref`, 1 `end`): per-object `call` sub-chains carrying UNPACKs, and call tags copied from the template at `0x00394A90`, whose upper half holds no VIFcode. (The earlier "254,000 call/ret sub-chains" was a misreading.) Dispatch counts: `FUN_00141EA0` runs 5 times per frame and returns early on empty lists ("reached once" came from `--stop-at-pc` stopping at the first hit); the per-object routine `FUN_00151180` runs about 81 times per frame but never calls `FUN_00149DD0`, which appends the object's matrix UNPACK and MSCAL 0 | `FUN_00151180` culls each object against the view volume with VU0 macro code (`FUN_00151960`, `FUN_001518F0`; matrix product `FUN_0013E790`) that scales by VF0.w. `R5900Context`'s constructor zeroed VF0, so VF0.w read 0 instead of 1 and every object failed the test. Upstream sets VF0 in its runtime, which we do not use | `R5900Context()` sets VF0 to (0, 0, 0, 1) (unit test "a new context's VF0 is (0, 0, 0, 1)"). Afterwards: 123,094 MSCALs and 301,100 MSCNTs by vblank 2600. The M3a RAM SHA-1 at the title becomes `a524a87d…` (the boot path reads VF0); the GS stream `af2abbd8…` is unchanged |
| VU1 kicks only empty GIF tags (B1, resolved 2026-10-22) | After the VF0 fix: 116,521 XGKICKs by vblank 2600, every one from micro address 0x0008 kicking the empty GIF tag at VU1 data qword 0x13 (copied from the template at `0x00394A80`); the 3D programs never kick. Per-program histogram: MSCAL 0x64 runs 2 cycles and ends at 0x330; the MSCNT from 0x330 runs about 150 cycles and ends at 0x2B0, inside the program at address 0; later MSCNTs from 0x2B0 end at once. A pair trace of one MSCNT from 0x330: the program adds an offset to a VI register and jumps through it with JALR in the next pair; the target should be 0x960 (an uploaded routine), but the interpreter jumped to 0 | JR and JALR followed the conditional branches' rule (a VI register written by the previous instruction reads as its old value). In PCSX2 the rule covers IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ and IBGEZ only, in both the microVU recompiler (`analyzeBranchVI` is called from the conditional-branch analysis alone) and the interpreter (`_vuJR` and `_vuJALR` read VI directly) | JR and JALR read the current VI value (`vu_lower.cpp`); the inherited test "JR shares the one-instruction VI branch visibility rule" is replaced by "JR and JALR jump through the VI value the previous instruction wrote". The field draws: terrain, characters, the wagon and the forest (frames at vblanks 2050–2300) |
| Field speed with VU1 drawing (B1, recorded 2026-10-22) | New Game probe, `--gs null`, `--movies blank`: the boot to vblank 1950 takes 4.6 s in the `release` preset (RelWithDebInfo); vblanks 1950–2300 in the field take 71 s, about 4.9 vblanks/s or 8 % of real time (Debug: about 0.6 vblanks/s). By vblank 2300 VU1 has run 185,685 programs, 383 M interpreter cycles and 879,802 XGKICKs. No profiler is installed (`perf` is missing), so the split between the VU1 interpreter and the PATH1 and GIF handling is not measured | The cost arrived with VU1's work; the interpreter models the pipeline cycle by cycle. Speed is recorded, not gated (owner, 2026-10-21) | None yet. Long segments (B3, B4) take tens of minutes a run |
| GetInfo after the first call (session D, 2026-10-22) | M3a acceptance input, `--movies blank`, the `release` preset with D's branch merged with `11b5a6a`, RAM at vblank 1200 against `11b5a6a` alone | M3's card answered GetInfo -2 every time; D's answers -2 once (a new, unformatted card), then 0. At the title four RAM words differ: the game's per-port record at `0x00467324`/`0x00467328` and its copy at `0x01BC858C`/`0x01BC8590` hold result 0 and "changed" 0 instead of -2 and 1. The title still comes at vblank 1200, the GS stream is `af2abbd8…`, the RAM SHA-1 becomes `ff521066…` | Expected (Protocol notes, libmc). B0's RAM hash moves to `ff521066…` with D |
| Stale-return check in every build (session D, 2026-10-22) | Six interleaved `release` runs each with and without the check, the M3a input to the title (`--gs null`), on a loaded 12-thread host | No measurable cost: best runs 2.220 s with, 2.227 s without for 1,200 vblanks; the spread between runs (2.2 to 4.4 s) is the host's load | The check stays on in Release |
| Does the movie player's nodata callback yield? (session C, 2026-10-22) | Ghidra: `0x001A0B00`, its callee `0x0019FD80`, the thread set-up `0x001A1A00` | Yes: RotateThreadReadyQueue at the player threads' common priority, then a staging-ring refill that starts DMA channel 4 only when the video callback has queued a slot, which the HLE never does | GetPicture calls it through `call_guest` (Protocol notes) |
| Delete's address (session C) | Ghidra: `0x00111380` and its one caller | `0x00111380` is sceMpegDelete (returns 1; called once from the stop routine `0x001A13D0` with the player's mp) | `delete` in `[movie.sce_mpeg]` confirmed |
| DemuxPssRing with a refusing callback (session C) | Ghidra: `0x001155C0`, the reader `0x001A0B40` | Stops at the refused packet and returns the bytes up to the last accepted one; the reader keeps the rest in its ring, so the packet is offered again | The video callback replacement refuses at four pending pictures |
| The open's name argument (session C) | Ghidra: `0x001A2450`, `0x001A1C60`, `0x0019F2F0`, `0x0019E430` | $a2 of the open, a C string (the name before the path and extension rewrite); $t0/$t1 width and height. The entry `0x001A2450` reaches the open with a tail jump, so a hook on the open never runs; the open's first call `0x0019E430` gets the name in $a0 | `[movie.sce_mpeg] player_open = 0x0019E430`, `player_open_name_arg = 0`; the `movie` events carry the name (`L5_CI_W.PSS` first) |
| How `.MVI` data arrives (session C) | Ghidra: `0x0019F130` → the file layer `0x0024E990` → `0x00162980`, `0x00161ED0`, `0x00161880` | The `cdrom0:` device: sceCdSearchFile, then sceCdRead by LSN through libcdvd's N-commands; not fileio, not CD streaming | None: M3's cdvdfsv already serves both |
| The first movie under the HLE (session C, 2026-10-22, real disc, Clang Debug, `boot-to-title-settings-only.dq8in`, `--gs trace`) | `--movies nodecode` and `play`; override call counts in `--json`; a temporary stderr log of the HLE (not committed) | The first movie is `L5_CI_W.PSS` (the developer logo, the 16:9 variant; the `movie` open event at vblank 1139 names it; the title flyover comes after it). The player starts its threads at vblank 1143 (threads 11–13, priority 40): Create, five AddCallback, two AddStrCallback (video and PCM). The video arrives as expected (the first payload starts with the sequence header, 512 × 448, not low-delay; PES PTS every few packets), backpressure refuses while four pictures wait, and pictures are delivered every second vblank from 1146 in I B B P order; with `play` dq8-movie decodes them with 0 errors and 0 tag mismatches (the first eleven pictures are black). At vblank 1162 the video stops arriving: the PCM stream callback `0x001A0470` is called (this movie has audio; S6a found 48,000 Hz PCM in the L5_CI pair), and with the sound modules as null servers the game's PCM ring never drains, so DemuxPssRing stops in front of it and the decoder waits in GetPicture's nodata loop for ever. Accepting and dropping the PCM packets instead (a local experiment) stalls earlier: the reader starts playback only when the audio preload threshold is reached | Open, B7 after A+B: movies need PCMPLAY's play position to advance (session A's interpreted PCMPLAY and session B's SPU2). Until then `--movies auto` with dq8-movie built (or `play`, `nodecode`) stops the boot at the developer logo; use `--movies skip` or `blank` |
| `--movies auto` with dq8-movie built (coordinator, 2026-10-22, at C's merge) | The default boot plays the developer logo `L5_CI_W.PSS` at vblank 1139 and waits there forever: its audio drains only through the sound modules, still null servers (session C's row above) | `auto` plays only when no profile IOP module is answered by a null server (`game::movie_audio_problem`); until bring-up step B8 interprets the sound modules it skips and says why in `movie_decoder {available: false, reason}`. `play` and `nodecode` are not affected | The default boot reaches the title again (vblank 1368 with the settings-only script); test "auto skips while a sound module is still a null server" |
| Where the field's time goes (B1 follow-up, 2026-10-22) | Stack samples of the probe above (gdb interrupting the `release` build; `perf` and `valgrind` are missing): about 94 % of the machine thread is in the VU1 interpreter. Self time: the pipeline commit pass 30 % (all 51 pipeline entries scanned twice per instruction), the hazard check 15 %, the run loop 9 %, operand normalization 7 % (one call per operand, across translation units), the hazard marks 6 %, the decoded-pair copy and the virtual code-generation read 6 %, the FMAC flag path about 10 % (x87 long double). Decode-cache rebuilds after each frame's MPG, VIF UNPACK and the PATH1 and GIF handling are small. Afterwards the GS stream's SHA-1 (`GsPipeline` hash) is about 8 % of the probe | Behaviour-preserving only: cycle counts, VU memory and the GS stream are unchanged. The probe's whole `--json` report at vblank 2300 is byte-identical before and after with `--gs null` and `--gs trace` (RAM SHA-1 `325a15bd…`; VU1 185,685 programs, 383,227,873 cycles and 879,802 XGKICKs; `gs_hash` `7dc25d23…`), and so is the 939 MB `--gs-trace` file (SHA-1 compared, file deleted). A differential fuzzer in scratch ran the old interpreter beside the new one on random VU1 programs (branches, XGKICK packets, D, T and E bits, reserved opcodes, cycle-budget slices, EE stores between slices) and on single FMACs with operands at the overflow and underflow boundaries, comparing state, data memory and packets after every slice; it catches seeded faults (a skipped commit cycle, products rounded to float, a reversed commit order, a short XGKICK copy). Release: the probe takes 74 s before and 31 s after, and vblanks 1950–2300 69 s and 27 s: 5.1 to 13 vblanks/s, 8 % to 22 % of real time (the boot to vblank 1950 stays 4.6 s). Debug (`dev-clang`): vblanks 1950–2300 take 300 s and 180 s, 1.2 to 1.9 vblanks/s. Measured back to back on the shared 12-core host | `vu_core.cpp`, `vu_interpreter.h`, `vu_upper.cpp`: entries commit from a 64-slot ring indexed by ready cycle (one 64-bit mask per slot, its bits in the old scan order), with a valid mask per pipeline array; cycles with nothing due and no XGKICK in flight are skipped; each decoded pair carries its hazard lists, written VI and latencies; the fetch returns a reference and checks the code generation only after a host call; the FMAC flags reuse execUpper's operands, decode the form once per instruction and classify products in double (exact for normalized floats; sums stay long double); `normalizeOperand` is inline and branchless (equal to the old one on all 2^32 inputs); XGKICK copies whole qwords and no longer clears its 64 KiB buffer per kick |
| IOP-to-EE RPC: SDRDRV binding the EE's server (B8, 2026-10-22) | LIBSD and SDRDRV as `interpret` rows: stop at vblank 11, "an RPC end packet for the IOP client structure 0x000193A0, which has no request in flight". `--trace iop,sif,rpc`: SDRDRV binds EE server 0x80000704, the EE's own RPC library replies with an end packet, and the IOP side rejects it | The IOP side matched end packets by client structure and rpc_id; the EE library's bind reply leaves the rpc_id its packet slot held. The IOP's own library matches by client alone | `IopExec::on_rpc_end` matches by client and checks the packet's command against the request (bind or call); the test harness's EE servers now answer binds with a stale id, as the game's library does |
| The sound modules interpreted (B8, 2026-10-22) | All ten sound rows `interpret` (LIBSD, SDRDRV, MODHSYN, MODMIDI, MODMSIN, SKSOUND, SKHSYNTH, SKMIDI, SKMSIN, PCMPLAY; CDVDSTM stays native): every module links with no unimplemented import and starts (SDRDRV returns 2, the others 0); LIBSD's initialisation gets past the SPU2 status poll and the IRQ wait that held it on session A's stand-in | The register-level SPU2 answers LIBSD's handshakes. M3a input, `--movies blank`: the title at vblank 1207; music from the title on (WAV peaks to 10,878); two runs give identical events and WAVs (audio hash `8e88d4a1…` at vblank 2400); the release preset needs 7.7 s for 2400 vblanks | `data/regions/SLUS_21207.toml`: the ten rows are `interpret`. Not yet compared with PCSX2's `SPU2.bin` or WAVs (acceptance item 2) |
| Movies with audio (B7 with B8, 2026-10-22) | `--movies play`, the settings-only script: the developer logo (`L5_CI_W.PSS`, 268 pictures, with audio) plays through and is deleted at vblank 1679; the flyover `TAKA_W.PSS` plays its 750 pictures; the title at vblank 3182. Frames at vblanks 1400, 2200 and 2900 show the logo and the flyover; the WAV has signal through the logo and from the flyover into the title | PCMPLAY's AutoDMA now drains the movie audio, so the stall in session C's row is gone. With no `null:` row left, `--movies auto` plays when dq8-movie loads (the default boot reaches the title at vblank 3182) | None. SSIM against PCSX2 frames and PCMPLAY's sample count are still to measure (acceptance item 3) |
| The field with sound (B8, 2026-10-22) | New Game probe, `--movies blank`, all sound rows interpreted, release preset: mode 0 at vblank 1698, VU1 drawing (159,743 programs by vblank 2300), field music in the WAV from 33 s; 29.7 s for the probe (31 s without sound) | The interpreted IOP and the SPU2 cost little next to VU1 | None |

## Results

*(Filled in at acceptance: counts, vblanks, SSIM, timings; addresses only.)*
