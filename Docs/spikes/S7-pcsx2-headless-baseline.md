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
