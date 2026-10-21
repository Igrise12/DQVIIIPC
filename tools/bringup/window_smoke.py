#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Smoke test of `dq8-game --window` with live input (M3b acceptance 9), on a private Xvfb display.

Starts Xvfb, runs dq8-game with an SDL3 window on it, and plays the boot to the title menu with either XTest key
presses (--input keyboard) or a temporary uinput gamepad that SDL sees as an Xbox 360 pad (--input gamepad; needs
write access to /dev/uinput, usually the `input` group; the device is removed when the run ends). It takes an
x11grab screenshot of the window after each step and stops dq8-game with SIGINT, which SDL turns into a window
close.

    python3 -I tools/bringup/window_smoke.py --iso "$DQ8_ISO" [--input keyboard|gamepad] [--out DIR]
        [--module-dir work/recomp/modules/SLUS_21207] [--dq8-game build/dev-clang/src/host/dq8-game]

Steps: settings screen; Down (Wide Screen); confirm (Sound); confirm; wait for the second movie skip; confirm (the
title fades in at once instead of after the 648-frame flyover timeline); Start (the menu); confirm (the
memory-card slot window). Prints each step's time, the input dq8-game recorded (--record-input) and its exit
code. Screenshots and logs are game-derived: keep them under work/ (the default), never in the repository.
Contributor tool only; Python 3 standard library, Xvfb, ffmpeg, and libX11/libXtst for the keyboard.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "pcsx2"))
from headless_run import Keyboard  # noqa: E402  (XTest key injection, shared with the PCSX2 harness)

SCREEN = "1280x960"

# Linux input and uinput constants (linux/input-event-codes.h, linux/uinput.h; x86-64 ioctl encodings).
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST = 0x130, 0x131, 0x133, 0x134
BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR = 0x136, 0x137, 0x13A, 0x13B, 0x13C, 0x13D, 0x13E
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y = 0, 1, 2, 3, 4, 5, 0x10, 0x11
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
UI_DEV_SETUP, UI_ABS_SETUP = 0x405C5503, 0x401C5504
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567


class VirtualPad:
    """A uinput gamepad with the USB ids of an Xbox 360 pad, so SDL's evdev backend maps it as one."""

    def __init__(self) -> None:
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
        for code in (BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
                     BTN_THUMBL, BTN_THUMBR):
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        for code, low, high in ((ABS_X, -32768, 32767), (ABS_Y, -32768, 32767), (ABS_RX, -32768, 32767),
                                (ABS_RY, -32768, 32767), (ABS_Z, 0, 255), (ABS_RZ, 0, 255), (ABS_HAT0X, -1, 1),
                                (ABS_HAT0Y, -1, 1)):
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            fcntl.ioctl(self.fd, UI_ABS_SETUP, struct.pack("<HHiiiiii", code, 0, 0, low, high, 0, 0, 0))
        fcntl.ioctl(self.fd, UI_DEV_SETUP,
                    struct.pack("<HHHH80sI", 0x03, 0x045E, 0x028E, 0x0114, b"Microsoft X-Box 360 pad", 0))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)

    def _emit(self, etype: int, code: int, value: int) -> None:
        os.write(self.fd, struct.pack("<qqHHi", 0, 0, etype, code, value))
        os.write(self.fd, struct.pack("<qqHHi", 0, 0, EV_SYN, 0, 0))

    def press(self, name: str, hold: float = 0.15) -> None:
        if name == "down":
            self._emit(EV_ABS, ABS_HAT0Y, 1)
            time.sleep(hold)
            self._emit(EV_ABS, ABS_HAT0Y, 0)
            return
        code = {"confirm": BTN_SOUTH, "start": BTN_START}[name]
        self._emit(EV_KEY, code, 1)
        time.sleep(hold)
        self._emit(EV_KEY, code, 0)

    def close(self) -> None:
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)


class Keys:
    """The keyboard table of src/runtime/platform/sdl/sdl_mapping.cpp: arrows, K = Cross, Return = Start."""

    def __init__(self, display: str) -> None:
        self.keyboard = Keyboard(display)

    def press(self, name: str, hold: float = 0.15) -> None:
        key = {"down": "Down", "confirm": "k", "start": "Return"}[name]
        self.keyboard.down(key)
        time.sleep(hold)
        self.keyboard.up(key)

    def close(self) -> None:
        self.keyboard.close()


def count_events(path: Path, kind: str) -> int:
    try:
        with path.open() as f:
            return sum(1 for line in f if f'"ev":"{kind}"' in line)
    except FileNotFoundError:
        return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--input", choices=("keyboard", "gamepad"), default="keyboard")
    parser.add_argument("--module-dir", default=str(REPO / "work/recomp/modules/SLUS_21207"))
    parser.add_argument("--dq8-game", default=str(REPO / "build/dev-clang/src/host/dq8-game"))
    parser.add_argument("--out", help="output directory (default work/bringup/window-<input>)")
    args = parser.parse_args(argv)
    out = Path(args.out or REPO / "work" / "bringup" / f"window-{args.input}")
    out.mkdir(parents=True, exist_ok=True)

    pad = VirtualPad() if args.input == "gamepad" else None
    read_fd, write_fd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", f"{SCREEN}x24", "-nolisten", "tcp"],
                            pass_fds=(write_fd,), stderr=(out / "xvfb.log").open("w"))
    os.close(write_fd)
    display = ":" + os.read(read_fd, 16).decode().strip()
    env = dict(os.environ, DISPLAY=display, SDL_VIDEO_DRIVER="x11")
    events = out / "events.jsonl"
    game = subprocess.Popen([args.dq8_game, "--iso", args.iso, "--module-dir", args.module_dir, "--window",
                             "--events", str(events), "--record-input", str(out / "recorded.dq8in"), "--json"],
                            env=env, stdout=(out / "summary.json").open("w"), stderr=(out / "game.err").open("w"))
    device = pad if pad is not None else Keys(display)
    start = time.monotonic()
    log: list[tuple[str, float]] = []

    def note(what: str) -> None:
        log.append((what, round(time.monotonic() - start, 2)))

    def shot(name: str) -> None:
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "x11grab", "-video_size",
                        SCREEN, "-i", display, "-frames:v", "1", str(out / f"{name}.png")], env=env, check=False)
        note(f"screenshot {name}.png")

    def press(name: str) -> None:
        device.press(name)
        note(f"press {name}")

    code: int | str = "not run"
    try:
        time.sleep(14.0)
        shot("settings")
        press("down")
        time.sleep(0.6)
        shot("settings-wide")
        press("confirm")
        time.sleep(1.0)
        shot("sound")
        press("confirm")
        deadline = time.monotonic() + 90
        while count_events(events, "movie_skip") < 4 and time.monotonic() < deadline and game.poll() is None:
            time.sleep(0.5)
        note("movies skipped" if count_events(events, "movie_skip") >= 4 else "movie skip not seen")
        time.sleep(1.0)
        press("confirm")
        time.sleep(4.0)
        shot("title")
        press("start")
        time.sleep(3.0)
        shot("menu")
        press("confirm")
        time.sleep(3.0)
        shot("slot")
    finally:
        wall = time.monotonic() - start
        game.send_signal(signal.SIGINT)
        try:
            code = game.wait(30)
        except subprocess.TimeoutExpired:
            game.kill()
            code = "killed"
        device.close()
        xvfb.terminate()
    for what, at in log:
        print(f"{at:7.2f} s  {what}")
    recorded = [line.strip() for line in (out / "recorded.dq8in").read_text().splitlines()
                if line.strip() and not line.startswith("#") and not line.startswith("dq8in")]
    print("recorded input:", "; ".join(recorded))
    try:
        machine = json.loads((out / "summary.json").read_text())["machine"]
        print(f"vblanks {machine['vblanks']} in {wall:.1f} s of wall time ({machine['vblanks'] / wall:.2f} per second)")
    except (ValueError, KeyError):
        print("no summary from dq8-game")
    print("dq8-game exit", code, "; screenshots in", out)
    return 0 if code == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
