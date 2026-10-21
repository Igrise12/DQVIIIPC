# tools/bringup/

Contributor tools for the M3 bring-up on the real disc (`Docs/milestones/M3-boots.md`, "Bring-up"). They read local
files (frames, and the events of a `dq8-game --events` run) or drive a local run. Nothing they produce goes into the repository. Frames,
dumps and logs stay under `work/`.

| Tool | Use |
|---|---|
| `ssim.sh ours.png reference.png [--crop W:H:X:Y] [--min 0.98]` | SSIM by S5b's method: both frames scaled bilinearly to 640×480, then ffmpeg's `ssim` filter. Prints the `All` value. With `--min` it exits 1 below the threshold. `--crop` uses 640×480 coordinates; M3b acceptance 8 uses it for the logo and text crop. Needs `ffmpeg`. |
| `python3 -I window_smoke.py --iso ISO [--input keyboard\|gamepad] [--out DIR]` | M3b acceptance 9: runs `dq8-game --window` on a private Xvfb display and plays the boot to the title menu with XTest key presses or a temporary uinput gamepad (an Xbox 360 pad to SDL; needs write access to `/dev/uinput`, usually the `input` group; removed when the run ends). Screenshots after each step, the input dq8-game recorded, the pacing and the exit code. Output defaults to `work/bringup/window-<input>/`. Needs Xvfb and ffmpeg. |
| `python3 -I rpc_trace.py events.jsonl [--sid SID] [--all] [--summary]` | The SIF, RPC and IOP events of a run, one line each, with server ids named as in the milestone's server table. `--summary` prints binds and calls per server, with first and last vblank. |

```bash
build/dev-clang/src/host/dq8-game --iso "$DQ8_ISO" --module-dir work/recomp/modules/SLUS_21207 --headless \
    --gs trace --input-script tests/local/input/boot-to-title.dq8in --until title --vblanks 6000 \
    --events work/bringup/run.jsonl --dump-frame 1200:work/bringup/settings.png
python3 -I tools/bringup/rpc_trace.py work/bringup/run.jsonl --summary
tools/bringup/ssim.sh work/bringup/settings.png work/pcsx2/<probe>/settings.png --min 0.98
```
