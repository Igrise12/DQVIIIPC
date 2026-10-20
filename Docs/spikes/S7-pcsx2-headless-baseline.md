# S7: Headless PCSX2 baseline: frame rate, resolution, GS usage, dumps and states

**Time box:** 2 sessions. **Depends on:** nothing. **Feeds:** S5b (dumps), S3 (title save state), brief Q7,
risk R3, decision D5. Runs **outside the sandbox** (Xvfb socket access), from the Terminal pane if needed.

## Why

We need ground truth from the reference emulator: the game's real update rate (30 or 60), its GS output
resolution and interlace mode, which GS features it leans on, how long it takes to reach the title, and a set
of GS dumps and save states at known points. PCSX2's own game index already hints at the renderer risk
(`autoFlush`, `halfPixelOffset: 2`, `roundSprite`, `nativeScaling: 2`, `cpuSpriteRenderBW`,
`cpuSpriteRenderLevel: 2` for SLUS-21207: bloom and depth-of-field through framebuffer-as-texture).

## Inputs and tools

- `DQ8_ISO`, `DQ8_PCSX2_APPRUN` (extracted PCSX2 2.7.524), `DQ8_PCSX2_APPIMAGE`, `DQ8_PS2_BIOS` from `tools/local.env`.
- Xvfb, ffmpeg, `python3` (stdlib only), XTest via ctypes.
- Pattern to port: `MGS2_Undub/src/mgs2_undub/emulator.py` and `docs/EMULATOR_NOTES.md`: unpack the AppImage
  only if `AppRun` is missing; private Xvfb (`-displayfd`); isolated `-datapath` (PCSX2 appends `PCSX2/`);
  `-testconfig` to initialise, then edit `inis/PCSX2.ini`; XTest key injection; `ffmpeg -f x11grab` screenshots;
  PCSX2 flags use one dash; `-version` exits 1 while printing the version.

## First-party deliverable: `tools/pcsx2/headless_run.py`

Profile settings written into the private ini: `EmuCore/GS Renderer=13` (software), `OsdShowFPS`,
`OsdShowResolution`, `OsdShowGSStats`, `OsdShowSpeed`, `OsdShowFrameTimes` = true, `GSDumpCompression=0`
(raw `.gs`: paraLLEl-GS reads uncompressed dumps only), `EmuCore SavestateZstdCompression=false` (so the Ghidra
extension's `PCSX2SaveStateImporter.java` can read states), memory card slot 1 enabled in the private profile
(the game asks for one), `EnableWideScreenPatches=false`, `EnableNoInterlacingPatches=false`, explicit `Pad1`
keyboard bindings, hotkeys for `Screenshot`, `GSDumpSingleFrame`, `GSDumpMultiFrame`, `SaveStateToSlot`,
`ToggleVideoCapture`. Each run writes `work/pcsx2/<scenario>-<id>/report.json` (settings, timings, screenshot
hashes, dump and state paths, emulog excerpts).

Scenarios (checkpoint screenshots at each step; generous waits; save a state at each checkpoint so later spikes
resume instead of replaying):

| Scenario | Reaches | Captures |
|---|---|---|
| `boot-title` | title screen | seconds-to-title, screenshot, **save state (TITLE.BIN resident, for S3)**, single-frame dump |
| `new-game-field` | first controllable field after the intro | screenshot, single + multi-frame dump, state |
| `menu` | main menu open | same |
| `battle` | first encounter (walk with a timeout) | same |

Also check whether the game's own config screen offers a 16:9 mode (relevant to the `_W` movies, brief §7).

## Measurements

- From the OSD in screenshots: internal resolution, GS frames vs vsyncs per second (30 vs 60), speed.
- From `emulog.txt`: `Set GS CRTC configuration` lines (width, height, interlace, field mode).
- From the dumps, with a first-party `tools/gsdump/gsdump_stats.py` (parse the PCSX2 dump header and packets,
  walk GIF tags): histogram of register writes incl. A+D targets; PSM values of FRAME/ZBUF/TEX0/BITBLTBUF;
  primitive types; alpha-blend formulas; TEST modes; mipmap and CLUT use; dithering; PABE/FBA; local-to-host
  readbacks; image-transfer bytes; draws per frame.

## Deliverables (findings)

Frame rate, resolution and interlace per scene; the GS feature histogram and what it implies for the backend
(architecture.md §6); time-to-title; VU0-micro corroboration if PCSX2 logs it; 16:9 option verdict; paths of
dumps and states under `work/pcsx2/` for S5b and S3. Update brief §6/§7 and tech-stack.md §12.

## Must not commit

Dumps, states, screenshots, videos, memory-card images, emulogs that print game text. The BIOS path lives only
in `tools/local.env`; the BIOS hash may be recorded.

## Findings

**Session:** 2026-10-19, two sessions (about 7 hours, of which 2 lost to CPU contention from parallel builds).
**Stop reason:** time box; boot-title, new-game-field, menu and the 16:9 checks are done, **battle not reached**
(see Open questions).

### Facts

**Harness and formats**

- `tools/pcsx2/headless_run.py` reproduces every run from `tools/local.env` alone: private Xvfb (`-displayfd`),
  isolated `-datapath work/pcsx2/profile` (PCSX2 appends `PCSX2/`), `-testconfig` then ini edits, XTest key
  injection, `ffmpeg -f x11grab` screenshots, optional OSD OCR with tesseract (`--ocr`), `--resume STATE.p2s`
  (PCSX2 `-statefile`), built-in scenarios `boot-title`, `new-game-field`, `menu` and a `probe` scenario driven
  by `--step` for exploration. Xvfb opens a display *inside* the sandbox here, so no Terminal-pane detour was
  needed — evidence: every `work/pcsx2/*/report.json`.
- PCSX2 2.7.524 ini keys that differ from the brief: save-state compression is `EmuCore/SavestateCompressionType = 0`
  (not `SavestateZstdCompression`); dumps `EmuCore/GS/GSDumpCompression = 0`; software renderer `Renderer = 13`;
  OSD keys `OsdShowFPS`, `OsdShowVPS`, `OsdShowSpeed`, `OsdShowResolution`, `OsdShowGSStats`, `OsdShowFrameTimes`.
  `-version` exits 1 while printing the version. Without a window manager `-fullscreen` is ignored and the game
  renders in a 640x480 window at the screen origin; the OSD stays legible.
- Uncompressed save state = zip with *stored* entries: `eeMemory.bin` 33,554,432 B, `iopMemory.bin` 2,097,152 B,
  `GS.bin` 4,194,813 B, `vu0MicroMem.bin` 4,096 B, `vu1MicroMem.bin` 16,384 B, `vu0Memory.bin`, `vu1Memory.bin`,
  `Scratchpad.bin`, `SPU2.bin`, `PCSX2 Internal Structures.dat`, `Screenshot.png` — readable with any zip
  reader, so the Ghidra importer needs no zstd.
- Raw GS dump container (2.7.524, `GSDumpCompression=0`): `u32 0xFFFFFFFF`, `u32 header_block_size` (36-byte
  header + serial + 640x480 BGRA screenshot = 1,228,846), header fields `state_version` **9**, `state_size`
  4,194,813, serial `SLUS-21207`, crc `F4715852`; then the GS state blob, 8,192 B of privileged registers,
  then packets: 0 transfer (`u8 path`, `u32 len`), 1 vsync (`u8 field`), 2 read-FIFO (`u32`), 3 registers
  (8,192 B). Every transfer carries path index 3 (PCSX2's unified GIF unit), so dumps cannot tell PATH1/2/3
  apart. State version 9 meets paraLLEl-GS's requirement (S5b). Each dump also writes a PNG screenshot beside
  the `.gs`. The multi-frame hotkey records a **fixed 26 vsyncs** (0.43 s) per press in this build; a single-frame
  dump holds 4 vsyncs. Parser: `tools/gsdump/gsdump_stats.py` (`--markdown`, `--table`, `--json`).
- Scripted input: the game samples the pad once per guest frame; a 0.15 s tap is lost below full speed, a 0.4 s
  hold of a D-pad direction auto-repeats (two cursor moves). Face buttons use 0.4 s, D-pad 0.15 s.

**Boot timeline** (fresh fast boot, 100 % speed; seconds since PCSX2 launch from
`work/pcsx2/boot-title-20261019-200258/{emulog.txt,report.json}` and `probe-20261019-182837`)

| t (s) | Event |
|---|---|
| 0.4 | CDVD open; serial SLUS-21207, PCSX2 game CRC `F4715852` (PCSX2's own CRC, not the zlib CRC-32 `a66a08b0` of brief §6) |
| 0.4 | PCSX2 creates an 8 MB *unformatted* memory card (slot 1 enabled in the private profile) |
| 2.0 | VM subsystems initialised (1.66 s) |
| 3.0 | `SLUS_212.07` executing, entry `0x00100008`; GameDB applies `autoFlush 1`, `halfPixelOffset 2`, `roundSprite 1`, `nativeScaling 2`, `cpuSpriteRenderBW 1`, `cpuSpriteRenderLevel 2` (hardware-renderer fixes; we run the software renderer) |
| 3.1 | IOP reboot with the disc's `IOPRP300.IMG`; `Set GS CRTC configuration. NTSC 640x448 @ 59.940 (59.82) Interlaced (FRAME)` |
| 3.2–4.1 | IOP modules in this order: SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, MODHSYN, MODMIDI, MODMSIN, CDVDSTM, SKSOUND (maxtrack 16), SKHSYNTH (maxentry 16), SKMIDI (maxtrack 5), SKMSIN (maxtrack 9), PCMPLAY. `MSIFRPC.IRX` is on the disc but never loaded |
| 4.2 | `Set GS CRTC configuration. NTSC 640x448 @ 59.940 (59.82) Interlaced (FIELD)`; stays so in every scene |
| 4.5–5.0 | Game configures the pad: analog **on and locked**, vibration on (`Pad: DS2 Config Finished … AL: On - AB: Locked - VS: Normal`) |
| ≈12 | Memory-card check screen |
| ≈16 | First-run **system settings** screen (Screen Size); it waits for input. Cross → Sound page (BGM/SE/VOICE 0–5, Stereo/Monaural); Cross again confirms |
| +0–3.5 | Square Enix logo (a movie: 53 PRIM, 4 draws per frame) |
| +4–17 | Level-5 logo (a movie: 560 PRIM, 5 draws) |
| +18–38 | In-engine flyover (503 PRIM, 7 draws) |
| **+38.3** | **Title screen** fades in (64.6 s after launch with the two confirms at 22 s and 26 s). With Cross pressed during the logos the title appears **+16 s** after the second confirm (`probe-20261019-195509`) |

The title screen idles for at least 100 s without starting an attract movie. Start opens the title menu
(three entries: continue, create and delete an adventure log), Cross confirms, Triangle cancels; continue and
delete ask for a memory-card slot. Create opens an 8-character name keyboard (confirmed on its End key), then asks
whether to save the adventure log to a memory card; **No** starts the game without a card. The opening movie
then runs ≈60 s (28–87 s after "No"), a load screen shows black at 16.7 ms frames, an in-engine cutscene with
dialogue boxes follows from ≈98 s, and after ≈45 s of Cross presses the game shows its controls Help screen and
hands over control at the camp beside the wagon (`probe-20261019-202044/dlg-2.png`). From title to field: about
4 minutes of guest time including the unskipped movie.

The game prints nothing on the EE console (no overlay-load messages); overlay evidence comes from the states.

**Controls as the game documents them (Help screen, `probe-20261019-202044/help-zoom.png`):** D-pad walk / move
cursor; left stick run / move cursor; right stick change camera angle; Cross confirm / perform action; Circle
open menu; Triangle cancel (closes the menu); Square view map; L1 / R1 rotate view left / right; L2 shift camera
behind the player; R2 toggle first-person view. Reachable later from the Misc. menu's Help option.

**Frame rate, resolution, interlace per scene** (OSD from screenshots whose Speed reads 100 %, CRTC from the
dumps' privileged registers)

| Scene | OSD internal res | OSD FPS / VPS | Dump kicks per vsync | Update rate | PCSX2 PRIM / DRW per frame |
|---|---|---|---|---|---|
| First-run settings | 512x448 NTSC Interlaced (Field) | 29.97 / 59.94 | 563, 0, 575, 0 | 30 Hz | 1,007–1,607 / 10 |
| Publisher logos, opening movie (MVI playback) | same | 29.97 / 59.94 | 500, 0 alternating over 26 vsyncs | 30 Hz | 53–560 / 4–5 |
| Title screen | same | 29.97 / 59.94 | 507, 0 alternating over 26 vsyncs | 30 Hz | 507 / 10 |
| Title menu, memory-card prompt | same | 29.97 / 59.94 | 1,579 / 1,010, 0 alternating | 30 Hz | 1,010–1,573 / 14–23 |
| In-engine cutscene (Yangus close-up) | same | 29.97 / 59.94 | — | 30 Hz | 17,190 / 1,360 |
| Field (camp by the wagon, first control) | same | 29.97 / 59.94 | 29,801–29,813, 0 alternating over 26 vsyncs | **30 Hz** | 17,950 / 1,249 (6,100–11,200 / 550–900 while walking) |
| Main menu (Circle) | same | **59.83 / 59.83** | 640 every vsync | **60 Hz** | 568–639 / 38–42 |
| Battle | **not reached** (three walking attempts, see Open questions) | — | — | — | — |

Every scene keeps the same CRTC state: SMODE2 INT=1 FFMD=0 (field mode); PMODE EN1=EN2=1, MMOD=1, ALP=127,
SLBG=0; both circuits read the **same** PSMCT24 buffer at FBP `0x070`, FBW 512, with DBY 0 and DBY 1; DISPLAY
DW=2559 DH=446/447 MAGH=4 MAGV=0 DX=636 DY=50 → a 512x448 frame shown as two 1-line-offset reads blended
50/50 (the game's own de-flicker). The displayed buffer **never changes** (0 DISPFB changes in every dump):
the game renders into FBP `0x000` (PSMCT32) and blits the result into the display buffer at `0x070` each
frame (the two PSMCT24 FRAME writes per frame plus a textured sprite reading TBP0 `0x0000`). PCSX2's
"640x448" in the log is the nominal video mode; the buffers are 512 pixels wide.

**GS VRAM map (inferred from the dumps):** `0x000`–`0x06f` colour target 512x448 PSMCT32 (917,504 B);
`0x070`–`0x0df` display buffer PSMCT24; `0x0e0`–`0x14f` depth PSMZ24; `0x150`–`0x1dc` small offscreen targets
in the field (FBW 192 and 64, some PSMCT16, FBMSK `0xff000000` and `0x0000ffff` channel masks) that are read
back as textures at TBP0 `0x2a00`–`0x3800` (= page × 32) — the post-processing (bloom / depth-of-field) that
PCSX2's `halfPixelOffset`/`nativeScaling` fixes exist for; `0x15a` a secondary target behind UI windows,
sampled at `0x2b40`; 4-bit UI textures as PSMT4HH / PSMT4HL in the unused high byte of the 24-bit Z-buffer
pages (`0x1c00` = `0x0e0` × 32), so a renderer must honour aliasing between depth and texture memory.

**GS feature histogram per scene** (`gsdump_stats.py --table`; counts are over the whole dump, per-frame
figures divide by the rendered frames; `work/pcsx2/scene-table.md`, `.stats.json`)

| metric | settings-screen | title-multi | field-multi | menu-multi |
|---|---|---|---|---|
| dump size (bytes) | 5,730,359 | 21,458,727 | 78,315,361 | 94,969,125 |
| vsyncs / rendered frames | 4 / 2 | 26 / 13 | 26 / 13 | 26 / 26 |
| update rate | alternate vsyncs (30 Hz) | alternate vsyncs (30 Hz) | alternate vsyncs (30 Hz) | every vsync (60 Hz) |
| display-buffer changes | 0 | 0 | 0 | 0 |
| vertex kicks per frame | 569 | 507 | 29,802 | 640 |
| state-change draws per frame (est.) | 65 | 10 | 4,793 | 165 |
| GIF tags per frame | 50 | 77 | 21,414 | 831 |
| primitives | SPRITE ×1128, LINESTRIP ×8, TRIANGLE ×2 | SPRITE ×6539, LINESTRIP ×52 | TRISTRIP ×244452, TRIFAN ×75581, LINE ×32912, TRIANGLE ×27677, SPRITE ×6747, LINESTRIP ×52 | SPRITE ×16536, LINESTRIP ×104 |
| PRIM flags (top) | SPRITE TME+ABE+AA1+FST ×8, SPRITE FST ×6, PRMODE:POINT TME+FST ×2, SPRITE ×2 | SPRITE TME+ABE+FST ×52, SPRITE FST ×39, PRMODE:POINT TME+FST ×13, SPRITE ×13 | TRIFAN ×41424, PRMODE:POINT IIP+ABE ×31869, PRMODE:POINT IIP+TME+FGE ×20332, TRISTRIP ×14716 | SPRITE TME+ABE+FST ×3380, SPRITE TME+ABE+AA1+FST ×104, SPRITE FST ×52, SPRITE IIP+ABE+AA1+FST ×52 |
| FRAME PSM | PSMCT32 ×32, PSMCT24 ×2 | PSMCT32 ×247, PSMCT24 ×13 | PSMCT32 ×7683, PSMCT16 ×78, PSMCT24 ×13 | PSMCT32 ×6058, PSMCT24 ×26 |
| FRAME FBW | 512 ×34 | 512 ×260 | 512 ×6591, 192 ×793, 64 ×351 | 512 ×6084 |
| FRAME FBMSK | 0x00000000 ×34 | 0x00000000 ×260 | 0x00000000 ×7696, 0xff000000 ×52, 0x0000ffff ×26 | 0x00000000 ×6084 |
| render-target pages | 2: 0x000 0x070 | 2: 0x000 0x070 | 18: 0x000 0x070 0x150 0x152 0x153 0x15b 0x15d 0x15f … | 3: 0x000 0x070 0x15a |
| ZBUF PSM / ZMSK | PSMZ24 ×34 / 1 ×18, 0 ×16 | PSMZ24 ×260 / 0 ×143, 1 ×117 | PSMZ24 ×7774 / 0 ×5473, 1 ×2301 | PSMZ24 ×6084 / 0 ×5460, 1 ×624 |
| TEX0 PSM | PSMT4HH ×120, PSMT8 ×4, PSMCT32 ×2 | PSMT8 ×52, PSMCT32 ×26 | PSMT8 ×3289, PSMCT32 ×455, PSMCT24 ×455, PSMT4 ×117, PSMCT16 ×26 | PSMT8 ×3432, PSMT4HL ×754, PSMCT32 ×52 |
| CLUT PSM / CSM / CLD | PSMCT32 ×124 / 0 ×124 / 1 ×124 | PSMCT32 ×52 / 0 ×52 / 1 ×52 | PSMCT32 ×3406 / 0 ×3406 / 1 ×3406 | PSMCT32 ×4186 / 0 ×4186 / 1 ×4186 |
| TEX0 TFX / TCC | MODULATE ×124, DECAL ×2 / 1 ×126 | MODULATE ×65, DECAL ×13 / 1 ×78 | MODULATE ×4303, HIGHLIGHT2 ×26, DECAL ×13 / 1 ×4329, 0 ×13 | MODULATE ×4212, DECAL ×26 / 1 ×4238 |
| TEX0 sizes | 512x512 ×122, 256x256 ×4 | 512x512 ×65, 512x64 ×13 | 256x256 ×1573, 128x128 ×962, 64x64 ×936, 512x512 ×624 | 256x128 ×1508, 256x256 ×1404, 512x512 ×806, 512x256 ×390 |
| texture bases in RT/Z pages | 2 | 1 | 13 | 2 |
| TEX1 (filter, MXL) | MXL=0 MMAG=NEAREST MMIN=NEAREST LCM=1 MTBA=1 ×122, MXL=0 MMAG=NEAREST MMIN=NEAREST LCM=0 MTBA=1 ×4 | MXL=0 MMAG=LINEAR MMIN=LINEAR LCM=0 MTBA=1 ×39, MXL=0 MMAG=NEAREST MMIN=NEAREST LCM=0 MTBA=1 ×26 | MXL=0 MMAG=LINEAR MMIN=LINEAR LCM=0 MTBA=1 ×3822, MXL=0 MMAG=LINEAR MMIN=LINEAR LCM=1 MTBA=1 ×507 | MXL=0 MMAG=NEAREST MMIN=NEAREST LCM=0 MTBA=1 ×2834, MXL=0 MMAG=NEAREST MMIN=NEAREST LCM=1 MTBA=1 ×780 |
| CLAMP | WMS=CLAMP WMT=CLAMP ×124 | WMS=CLAMP WMT=CLAMP ×65 | WMS=CLAMP WMT=CLAMP ×4017, WMS=REPEAT WMT=REPEAT ×312 | WMS=CLAMP WMT=CLAMP ×4212 |
| ALPHA formulas | (Cs - Cd) * As + Cd ×20, (Cs - Cd) * FIX + Cd [FIX=128] ×2 | (Cs - Cd) * As + Cd ×130, (Cs - Cd) * FIX + Cd [FIX=128] ×13 | (0 - Cs) * As + Cd ×16281, (Cs - 0) * As + Cd ×8828, (Cs - Cd) * As + Cd ×4745, (Cs - 0) * FIX + Cd [FIX=128] ×234 | (Cs - Cd) * As + Cd ×3640, (Cs - Cd) * FIX + Cd [FIX=128] ×26 |
| TEST | ATE=off DATE=off ZTE=GREATER ×14, ATE=ALWAYS/0/KEEP DATE=off ZTE=ALWAYS ×14, ATE=ALWAYS/0/KEEP DATE=off ZTE=GEQUAL ×4, ATE=off DATE=off ZTE=off ×2 | ATE=off DATE=off ZTE=GREATER ×130, ATE=ALWAYS/0/KEEP DATE=off ZTE=ALWAYS ×104, ATE=off DATE=off ZTE=off ×13, ATE=ALWAYS/0/KEEP DATE=off ZTE=GEQUAL ×13 | ATE=off DATE=off ZTE=GREATER ×2496, ATE=GEQUAL/1/KEEP DATE=off ZTE=GEQUAL ×1573, ATE=GEQUAL/64/KEEP DATE=off ZTE=GEQUAL ×1573, ATE=off DATE=off ZTE=ALWAYS ×1300 | ATE=ALWAYS/0/KEEP DATE=off ZTE=ALWAYS ×3562, ATE=off DATE=off ZTE=GREATER ×2444, ATE=off DATE=off ZTE=ALWAYS ×52, ATE=off DATE=off ZTE=off ×26 |
| uploads (TRXDIR) | host->local ×8 | host->local ×156 | host->local ×1625, local->local ×39 | host->local ×3068 |
| upload sizes (top) | upload 16x16 ×4, upload 128x128 ×2, upload 64x64 ×2 | upload 16x16 ×78, upload 256x224 ×65, upload 256x32 ×13 | upload 16x16 ×767, upload 64x64 ×338, upload 32x32 ×195, upload 128x128 ×156 | upload 16x16 ×1534, upload 128x128 ×520, upload 128x64 ×520, upload 64x64 ×234 |
| image bytes per vsync | 41,984 | 592,896 | 1,056,352 | 3,320,832 |
| readback pixels / ReadFIFO | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| local copies (pixels) | 0 | 0 | 194,818 | 0 |
| DTHE / PABE / FBA / COLCLAMP | 0 ×14 / none / 0 ×20 / 1 ×14 | 0 ×130 / none / 0 ×130 / 1 ×130 | 0 ×2496 / none / 0 ×5278 / 1 ×2496 | 0 ×2444 / none / 0 ×3640 / 1 ×2444 |
| PRMODECONT / FOGCOL / FOG writes | 1 ×20, 0 ×14 / none / 0 | 0 ×130, 1 ×130 / none / 0 | 0 ×2496, 1 ×2132 / 0x8cc3d7 ×34489, 0x000000 ×52 / 0 | 1 ×3640, 0 ×2444 / none / 0 |
| TEXA | TA0=128 AEM=0 TA1=128 ×4 | TA0=128 AEM=0 TA1=128 ×26 | TA0=128 AEM=1 TA1=128 ×676, TA0=128 AEM=0 TA1=128 ×26 | TA0=128 AEM=0 TA1=128 ×52 |
| CRTC | NTSC interlaced FIELD(FFMD=0); MMOD=1 ALP=127 SLBG=0 AMOD=0 | NTSC interlaced FIELD(FFMD=0); MMOD=1 ALP=127 SLBG=0 AMOD=0 | NTSC interlaced FIELD(FFMD=0); MMOD=1 ALP=127 SLBG=0 AMOD=0 | NTSC interlaced FIELD(FFMD=0); MMOD=1 ALP=127 SLBG=0 AMOD=0 |

What this says about the renderer (architecture.md §6):

- Field: ≈29,800 vertex kicks and ≈21,400 GIF tags per frame at 30 Hz: tri-strips and fans dominate, plus
  ≈2,500 LINE primitives per frame (grass) and Gouraud, fogged (PRMODE IIP+TME+FGE, FOGCOL `0x8cc3d7`) geometry.
  Blends are mostly **subtractive** `(0 - Cs) * As + Cd` and **additive** `(Cs - 0) * As + Cd`, then the
  standard formula; alpha test GEQUAL 1 / GEQUAL 64 for cut-outs; ZTE GEQUAL; no destination alpha test.
- Textures: PSMT8 with PSMCT32 CLUTs (CSM1, CLD 1) dominate, some PSMT4, PSMCT24/16 and PSMCT32; bilinear,
  **no mipmaps anywhere (MXL=0)**, MTBA=1, mostly CLAMP, TEXA AEM=1 for 24/16-bit; TFX MODULATE, rare DECAL and
  HIGHLIGHT2. Texture uploads ≈1.0 MB per vsync in the field (767 16x16 CLUT uploads plus 32–256 px textures)
  and ≈3.3 MB per vsync in the menu (it re-uploads its artwork every 60 Hz frame); the title re-uploads a
  256x224 backdrop every frame.
- **Movie playback (for S6):** during the opening movie the game uploads exactly 917,504 bytes per vsync =
  one full 512x448 PSMCT32 frame, as 896 host-to-local transfers of 16x16 pixels (IPU macroblocks), into the
  colour target, and draws it with 500 sprite kicks every other vsync; no other geometry, no offscreen targets,
  same CRTC as everywhere else (`probe-20261019-203908/movie-multi.gs`). So the decoded movie frame is
  512x448, 32-bit, and the `_W` variants presumably only change the content, not the GS path.
- **No local-to-host readbacks and no ReadFIFO in any scene**; 39 local-to-local copies of 127x34..44 px per 26
  vsyncs in the field. No dithering (DTHE 0), no PABE, no FBA, COLCLAMP 1, no SCANMSK, no SIGNAL/LABEL.
- Context 2 is never used; PRMODECONT toggles every frame (PRIM and PRMODE both carry the draw flags).

**VU0 micro mode (brief Q7, architecture.md §5.4):** `vu0MicroMem.bin` holds a microprogram in every state
(887 non-zero bytes, 132 of 512 64-bit words, last code byte at `0x6df`); VU1 micro memory is fully populated
(2,048 of 2,048 words). VU0 data memory is **all zero** at the settings screen, the title and the field
(`settings-screen.p2s`, `after-100s.p2s`, `field.p2s` — compared by hash), so the loaded VU0 program had not
written VU0 memory by then; whether it runs in battle is still open.

**Overlay window (for S3):** in every state the window `0x461C80..0x489A80` starts with the `MWo3` magic and the
header of overlay id 6 (TITLE): `load_address 0x00461C80`, `text_size 0x4AC0`, `data_size 0x680`,
`bss_size 0x600`; `64 + text + data` = 20,864 = the size of `BIN/TITLE.BIN`, so the whole file including its
header is resident, as brief §6 predicted. **TITLE.BIN is resident from the first-run settings prompt on** and
still at the field; the field state (`probe-20261019-202427/field.p2s`) had 15,862 non-zero bytes in the window
(MENU.BIN was never loaded into it before the main menu was opened — see `menu.p2s` for the menu case).

**Native 16:9:** the first-run settings screen has a **"Screen Size"** option with the values **"Normal 4:3"**
and **"Wide Screen 16:9"** (the prompt says settings can be changed later from the menu). The title screen and
title menu render **identically** in both modes (same CRTC, 507 kicks per frame, same uploads;
`boot-title-20261019-200258/title.gs` vs `probe-20261019-203255/title-wide.gs`), so the setting does not change
the CRTC or the 2D path. In the **field** the 16:9 mode is **anamorphic**: the same 512x448 buffer, the same
CRTC and DISPLAY registers, the same uploads, but a wider horizontal field of view squeezed into the frame
(characters visibly narrower, more scenery in view: 31,491 vs 29,802 vertex kicks and 18,448 vs 17,958 OSD
primitives per frame at the same spot; `new-game-field-20261019-204003/field-multi.gs` vs
`probe-20261019-202427/field-multi.gs`, `grid-wide-field.png`). The TV is expected to stretch it. For the port this
means a true 16:9 presentation needs only the game's own mode plus a 16:9 output aspect, no projection patch;
HUD placement in that mode is unverified (menu and text boxes looked unchanged in the frames taken).

**Disc layout for CDVD-read correlation** (ISO 9660 extents, 2,048-byte sectors): `SLUS_212.07` LBA 350–1795;
`SYSTEM.CNF` 349; `DATA2.HD6` 2119–2161; `DATA.HD6` 2162–2254; `MODULES/IOPRP300.IMG` 1796–1930 and the IRX
files 1931–2118; `DATA2.DAT` 760313–1127974; `BIN/BATTLE.BIN` 1134883–1134959; `BIN/CASINO.BIN` 1134960–1135023;
`BIN/MENU.BIN` 1135024–1135077; `BIN/TITLE.BIN` 1135078–1135088; `BIN/SHOP.BIN` 1135089–1135130;
`BIN/VIEWER.BIN` 1135131; `DATA.DAT` 1135132–2016097. `headless_run.py --verbose-cdvd` makes PCSX2 log every
sector read, so a read in 1135078–1135088 is TITLE.BIN being loaded.

**Caveats**

- Timings come only from runs whose OSD shows Speed 100 %. While sibling spikes compiled (load average 20–41,
  compiler jobs at nice −4), PCSX2 ran at 1–13 %, took 79 s to open the GS, and pad taps were not sampled; those
  runs (`probe-20261019-183906`) are kept only as failure-mode evidence. The coordinator paused S1's compile
  for the scenario runs.
- The private memory card stays unformatted (the save was declined), so every fresh boot shows the settings
  prompt; a formatted card with saved settings would reach the title ≈10 s sooner (no prompts).
- The hero was named with keyboard input from the harness; no save data exists.
- Software renderer only; the OSD "DRW" counts are PCSX2's flushes, the table's "draws (est.)" counts
  state-change-separated batches in our parser, so they differ by design.

### What changes in the docs
- project-brief.md §6: add rows: update rate 30 Hz (60 Hz in the main menu) at 60 Hz field-interlaced output,
  512x448, PSMCT24 single display buffer blitted from a PSMCT32 target, 50/50 field blend through PMODE ALP=127;
  the native 16:9 option; PCSX2 game CRC `F4715852` beside the CRC-32; analog-locked DualShock 2 mode; IOP module
  load order and arguments; VU0 micro memory populated; TITLE.BIN resident at the first-run prompt; the game's
  control mapping. Mark the overlay header semantics "confirmed from save states" for TITLE.
- project-brief.md §7 "True widescreen": replace "Research needed" with "the game has a native 16:9 mode
  (Screen Size: Wide Screen 16:9); expose it as a setting, verify HUD/movie behaviour, then decide whether an
  ultrawide patch is still wanted". "Frame interpolation": the native rate is 30 Hz (menu 60), so interpolation
  to 60 is the relevant target. §7 Input: the mapping above is the baseline; pressure is not used (AB locked).
- architecture.md §5.4: VU0 micro mode is used at least by loading microcode; plan a VU0 micro interpreter or
  AOT path, not only macro-mode helpers. §6.3: the presentation pipeline must implement PMODE circuit merging
  (two circuits, ALP blend, 1-line offset) and field-mode reads of a 24-bit buffer; the backend needs
  framebuffer-as-texture, 16-bit targets with FBMSK channel masks, and texture/depth memory aliasing from day
  one; mipmaps and readbacks can be left out of the first native renderer.
- tech-stack.md §12: Xvfb works inside the sandbox; `tesseract` is installed (OSD OCR); add the harness and the
  dump parser to the contributor tooling table.

### Decision impact
- D5 / ADR (first GS backend): keep paraLLEl-GS first; it already replays version-9 dumps and does PCRTC merging.
  The native renderer later must handle offscreen targets re-read as textures, PSMT4HH/HL aliasing into Z pages,
  subtractive/additive blends and alpha test; it can skip mipmapping, dithering, PABE, FBA and readbacks.
- R3: unchanged severity; the field already runs ≈30 k kicks and ≈1 MB of uploads per vsync, so the texture
  cache and upload path matter as much as rasterisation.

### Local artifacts (git-ignored)
- `work/pcsx2/profile/`: private PCSX2 data path (ini, 8 MB unformatted memory card, cache).
- `work/pcsx2/probe-20261019-182837/`: fresh boot, 35 OSD screenshots, `emulog.txt`, OCR'd `report.json`.
- `work/pcsx2/probe-20261019-183359/settings-screen.{gs,p2s,stats.json}`: first-run settings screen
  (**TITLE resident**, usable for S3), 5.7 MB dump, 42 MB state.
- `work/pcsx2/boot-title-20261019-200258/`: the baked `boot-title` run: `boot-000..037.png` (1 s cadence),
  `title.{p2s,gs,png}`, `title-multi.gs`, `title-multi-b.gs` (26 vsyncs each), `title-native.png`,
  `title-menu.{p2s,gs}` (memory-card prompt), `title.stats.json`, `contact-boot.png`.
- `work/pcsx2/boot-title-20261019-195838/`: earlier run; `level5-logo.{p2s,gs}`, `level5-logo-multi.gs`,
  `title-menu.{p2s,gs}` (title screen at 88 s / 93 s).
- `work/pcsx2/probe-20261019-195509/after-40s.p2s`, `after-100s.p2s`: title screen states; `probe-20261019-20{0556,0810,1008,1226,1500,1738,2044}/`:
  the new-game exploration (title menu, name keyboard, save prompt, movie, cutscene, dialogue) with a state at
  each step (`create-end`, `confirm`, `after-no`, `open-45`, `open-end`, `dlg-end`, `tail-end`).
- `work/pcsx2/probe-20261019-202427/`: **field** `field.{p2s,gs,png}`, `field-multi.gs` (78 MB, 26 vsyncs),
  `field-native.png`, duplicates `field-b.*`, **menu** `menu.{p2s,gs,png}`, `field-menu.stats.json`.
- `work/pcsx2/menu-20261019-203216/`: the baked `menu` run: `menu.{p2s,gs}`, `menu-multi.gs` (95 MB, 26 vsyncs), `menu-native.png`.
- `work/pcsx2/probe-20261019-203255/`: 16:9 title: `title-wide.{p2s,gs,stats.json}`, `title-wide-multi.gs`, `title-menu-wide.gs`.
- `work/pcsx2/probe-20261019-202605/`, `-202857/`, `-203605/`: walking attempts (no encounter), `walk-end.p2s`, `walk3-end.p2s`.
- `work/pcsx2/probe-20261019-203908/`: opening movie `movie.{p2s,gs}`, `movie-multi.gs` (31 MB, 26 vsyncs).
- `work/pcsx2/new-game-field-20261019-204003/`: the baked `new-game-field` run replayed in **16:9** from
  `title-wide.p2s` (275 s): `field.{p2s,gs,png}`, `field-multi.gs`, `field-native.png`, `opening-000..021.png`.
- `work/pcsx2/wide-movie.stats.json`: statistics of the 4:3 field, the 16:9 field and the movie dumps.
- `work/pcsx2/scene-table.md`, `scene-table.stats.json`: the comparison table above and its data.

### Open questions
- **Battle** (BATTLE.BIN resident, VU0 in battle, battle frame rate, depth-of-field): three walking attempts
  (≈8 minutes of movement, every 30 s straight run ending at the same cliff edge, Cross presses near the party
  without effect) show the starting camp is an enclosed clearing with no encounters; the exit is not along the
  headings tried. Next session: find the exit with the map (Square) or follow the stream, reach the road where
  encounters occur, then `--scenario battle` (to be written once the route is known): state + dumps at the
  first encounter and in the command menu. Until then the GS histogram lacks the battle scene.
- 16:9 HUD and text-box placement in the field: unverified beyond the frames above (M3 renderer work).
- Does VU0 microcode execute during gameplay or battle? → compare `vu0Memory.bin` across the battle state;
  S2/S3 find the `VCALLMS`/`VCALLMSR` sites.
- Movie audio/video sync and the IPU bitstream itself are outside the GS dump; S6 has the frame size and the
  upload pattern above.
