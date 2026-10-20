#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Headless, reproducible PCSX2 runs for spike S7 (Docs/spikes/S7-pcsx2-headless-baseline.md).

Contributor tool only; never on the end-user path. Python 3 standard library, Xvfb, ffmpeg and XTest.
Every run lives under work/pcsx2/ (git-ignored):

  work/pcsx2/profile/PCSX2/      private PCSX2 data path (ini, memory card, snaps, sstates, videos, logs)
  work/pcsx2/<scenario>-<id>/    one run: report.json, emulog.txt, screenshots, copied dumps and states

Paths come from tools/local.env: DQ8_ISO, DQ8_PCSX2_APPRUN or DQ8_PCSX2_APPIMAGE, DQ8_PS2_BIOS.
The user's own PCSX2 profile (~/.config/PCSX2) is never read or written.

Examples:
  python3 -I tools/pcsx2/headless_run.py --scenario boot-title
  python3 -I tools/pcsx2/headless_run.py --scenario battle --resume work/pcsx2/new-game-field-X/field.p2s
  python3 -I tools/pcsx2/headless_run.py --scenario probe --resume STATE --step wait:5 --step shot:a --step press:k

Step language (one string each; also used by the built-in scenarios):
  wait:S                 sleep S seconds (the emulator keeps running; aborts if it exits)
  press:KEY[:HOLD]       XTest key press (X keysym name: Return, k, Up, F1 ...), held HOLD seconds (default .4;
                         the game samples the pad once per guest frame, so a short tap is lost when the host is
                         loaded and PCSX2 runs below full speed)
  mash:KEY:S[:INTERVAL]  press KEY repeatedly for S seconds (advance dialogue)
  shot:NAME              ffmpeg x11grab screenshot of the Xvfb screen (includes the OSD) -> NAME.png
  probe:NAME:S:INTERVAL  one shot every INTERVAL seconds for S seconds -> NAME-NNN.png (time-to-X measurements)
  snap:NAME              PCSX2's own screenshot hotkey (no OSD) -> NAME.<ext>
  dump:NAME              single-frame raw GS dump -> NAME.gs
  mdump:NAME[:S]         multi-frame raw GS dump (fixed 26 vsyncs in PCSX2 2.7.524), S s to write -> NAME.gs
  state:NAME             uncompressed save state to slot 1, copied to NAME.p2s
  rec:NAME:S             PCSX2 video capture for S seconds -> NAME.mp4
  mark:LABEL             record the elapsed time under LABEL
  waitlog:REGEX:TIMEOUT  wait until emulog.txt matches REGEX (or TIMEOUT seconds)

Every run first waits until the guest runs (ELF entry on a fresh boot, GS thaw on --resume; up to 600 s on a
loaded host) before executing its steps, and records that time under marks.game-running.
"""
from __future__ import annotations

import argparse
import configparser
import ctypes
import ctypes.util
import hashlib
import json
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
WORK = ROOT / "work" / "pcsx2"
LOCAL_ENV = ROOT / "tools" / "local.env"
SCREEN = "1280x720"
DEFAULT_HOLD = 0.4

# Hotkeys we drive. Every PCSX2 default that would collide with these or with the pad bindings is moved away.
HOTKEYS = {
    "Screenshot": "F8",
    "GSDumpSingleFrame": "F7",
    "GSDumpMultiFrame": "F6",
    "SaveStateToSlot": "F1",
    "ToggleVideoCapture": "F10",
}
HOTKEY_PARKING = {  # defaults that sit on keys we press, or that must never fire by accident
    "CycleAspectRatio": "Keyboard/Control & Keyboard/F12",
    "CycleInterlaceMode": "Keyboard/Control & Keyboard/F11",
    "LoadStateFromSlot": "Keyboard/Control & Keyboard/F3",
    "NextSaveStateSlot": "Keyboard/Control & Keyboard/F2",
    "PreviousSaveStateSlot": "Keyboard/Control & Keyboard/Shift & Keyboard/F2",
    "ToggleSoftwareRendering": "Keyboard/Control & Keyboard/F9",
    "ToggleFrameLimit": "Keyboard/Control & Keyboard/F4",
    "TogglePause": "Keyboard/Control & Keyboard/Space",
    "ToggleTurbo": "Keyboard/Control & Keyboard/Tab",
    "HoldTurbo": "Keyboard/Control & Keyboard/Period",
    "OpenPauseMenu": "Keyboard/Control & Keyboard/Escape",
    "ToggleFullscreen": "Keyboard/Control & Keyboard/Alt & Keyboard/Return",
    "InputRecToggleMode": "Keyboard/Control & Keyboard/Shift & Keyboard/R",
}
# DualShock 2 layout on the keyboard (PCSX2's defaults, written explicitly so a run never depends on them).
PAD1 = {
    "Type": "DualShock2",
    "Up": "Keyboard/Up", "Right": "Keyboard/Right", "Down": "Keyboard/Down", "Left": "Keyboard/Left",
    "Triangle": "Keyboard/I", "Circle": "Keyboard/L", "Cross": "Keyboard/K", "Square": "Keyboard/J",
    "Select": "Keyboard/Backspace", "Start": "Keyboard/Return",
    "L1": "Keyboard/Q", "L2": "Keyboard/1", "R1": "Keyboard/E", "R2": "Keyboard/3",
    "L3": "Keyboard/2", "R3": "Keyboard/4",
    "LUp": "Keyboard/W", "LRight": "Keyboard/D", "LDown": "Keyboard/S", "LLeft": "Keyboard/A",
    "RUp": "Keyboard/T", "RRight": "Keyboard/H", "RDown": "Keyboard/G", "RLeft": "Keyboard/F",
}
# Lines of emulog.txt that may be quoted in a report: PCSX2's own messages, never the game's console output.
LOG_EXCERPT = re.compile(
    r"PCSX2 v|Savestate version|Set GS CRTC configuration|Opening CDVD|ELF \(|Game CRC|Serial|"
    r"McdSlot|\(FileMcd\)|GS Dump|GS dump|Saving GS|Saved GS|State saved|Saving state|Loading state|"
    r"Loading savestate|savestate|Screenshot|Video capture|Found patch|Applied|game fixes|GameFix|"
    r"Renderer|Software|Switching|Video Mode|Interlac|Frame Rate|VU0|VU1|MTVU|EE/iR5900|Loading ELF|"
    r"Loading .*\.IRX|Loading .*\.irx|IOP|Overlay|memcard|Memory Card|VsyncRate|vsync|Patch",
    re.IGNORECASE,
)
# Scenarios: step lists that reproduce the S7 captures. Each saves a state at its checkpoint so the next
# scenario resumes from it (--resume) instead of replaying the inputs.
SCENARIOS: dict[str, list[str]] = {
    "probe": [],
    # Fresh boot. With an unformatted memory card the game first shows its system settings (Screen Size, then
    # Sound), each confirmed with Cross; the publisher logos and the title follow without further input.
    "boot-title": [
        "wait:16", "shot:settings-screen",
        "press:k", "wait:3", "shot:settings-sound",
        "press:k", "mark:settings-confirmed",
        "probe:boot:60:1",
        "shot:title", "state:title", "dump:title", "mdump:title-multi:2", "snap:title-native",
        "press:Return", "wait:3", "shot:title-after-start",
        "press:k", "wait:3", "shot:title-after-cross", "state:title-menu", "dump:title-menu",
    ],
    # Resume from boot-title's title.p2s. Title menu: Start opens it, Cross confirms, Triangle cancels, Circle
    # opens the in-game menu (the game's own Help screen). D-pad taps are short: a 0.4 s hold auto-repeats.
    # The hero is named with one letter; End is reached by wrapping left from the second keyboard row; the
    # memory-card save is declined so the private card stays unformatted and boot-title stays reproducible.
    "new-game-field": [
        "press:Return", "wait:2", "shot:title-menu",
        "press:Down:0.15", "wait:1", "shot:title-menu-create",
        "press:k", "wait:3", "shot:name-entry",
        "press:k", "wait:1",
        "press:Down:0.15", "wait:0.5", "press:Left:0.15", "wait:0.5", "shot:name-end",
        "press:k", "wait:3", "shot:save-prompt",
        "press:Down:0.15", "wait:0.5", "press:k", "mark:new-game-started",
        "probe:opening:110:5",
        "mash:k:130:1.5", "mark:dialogue-mashed",
        "shot:field", "state:field", "dump:field", "mdump:field-multi", "snap:field-native",
    ],
    # Resume from new-game-field's field.p2s (or any field state).
    "menu": [
        "shot:field", "press:l", "wait:2",
        "shot:menu", "state:menu", "dump:menu", "mdump:menu-multi", "snap:menu-native",
        "press:i", "wait:1.5", "shot:menu-closed",
    ],
    # Not written yet: three walking attempts never left the enclosed starting camp (no encounters there).
    # Use --scenario probe with --step to find the exit, then define the route here (S7 findings, Open questions).
    "battle": [],
}


def load_local_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key.strip()] = value
    for key in list(values):
        if key in os.environ:
            values[key] = os.environ[key]
    return values


def prepare_emulator(env: dict[str, str]) -> Path:
    """Use the extracted AppRun if present (read-only); otherwise extract the AppImage once into work/pcsx2/app."""
    apprun = Path(env.get("DQ8_PCSX2_APPRUN", ""))
    if apprun.is_file() and os.access(apprun, os.X_OK):
        return apprun
    appimage = Path(env.get("DQ8_PCSX2_APPIMAGE", ""))
    install = WORK / "app"
    extracted = install / "squashfs-root" / "AppRun"
    if not extracted.is_file():
        if not appimage.is_file():
            raise FileNotFoundError("neither DQ8_PCSX2_APPRUN nor DQ8_PCSX2_APPIMAGE points at PCSX2")
        install.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(appimage.resolve()), "--appimage-extract"], cwd=install, check=True, timeout=120,
                       stdout=subprocess.DEVNULL)
    return extracted


def profile_settings(bios: Path, verbose_cdvd: bool) -> dict[str, dict[str, str]]:
    hotkeys = {name: f"Keyboard/{key}" for name, key in HOTKEYS.items()}
    hotkeys.update(HOTKEY_PARKING)
    return {
        "UI": {"SetupWizardIncomplete": "false", "ConfirmShutdown": "false", "PauseOnFocusLoss": "false",
               "HideMouseCursor": "true", "InhibitScreensaver": "false", "StartFullscreen": "true"},
        "Folders": {"Bios": str(bios.resolve().parent)},
        "Filenames": {"BIOS": bios.name},
        "MemoryCards": {"Slot1_Enable": "true", "Slot1_Filename": "Mcd001.ps2", "Slot2_Enable": "false"},
        "EmuCore": {"SaveStateOnShutdown": "false", "EnableDiscordPresence": "false", "EnableFastBoot": "true",
                    "EnableWideScreenPatches": "false", "EnableNoInterlacingPatches": "false",
                    "EnablePatches": "true", "EnableGameFixes": "true",
                    # 0 = uncompressed zip entries (2.7.524 key; older builds call it SavestateZstdCompression)
                    "SavestateCompressionType": "0", "BackupSavestate": "false",
                    "WarnAboutUnsafeSettings": "false", "CdvdVerboseReads": "true" if verbose_cdvd else "false"},
        "EmuCore/GS": {"Renderer": "13", "OsdShowFPS": "true", "OsdShowVPS": "true", "OsdShowSpeed": "true",
                       "OsdShowResolution": "true", "OsdShowGSStats": "true", "OsdShowFrameTimes": "true",
                       "OsdShowIndicators": "true", "OsdShowVersion": "true", "OsdScale": "100",
                       "GSDumpCompression": "0", "ScreenshotFormat": "0", "ScreenshotSize": "0",
                       "EnableVideoCapture": "true", "EnableAudioCapture": "true", "CaptureContainer": "mp4",
                       "AspectRatio": "Auto 4:3/3:2", "VsyncEnable": "false"},
        "SPU2/Output": {"Backend": "Null"},
        "Logging": {"EnableFileLogging": "true", "EnableTimestamps": "true", "EnableVerbose": "true",
                    "EnableEEConsole": "true", "EnableIOPConsole": "true"},
        "Hotkeys": hotkeys,
        "Pad1": PAD1,
    }


def configure_profile(profile: Path, settings: dict[str, dict[str, str]]) -> Path:
    config = profile / "PCSX2" / "inis" / "PCSX2.ini"
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str  # type: ignore[assignment]
    if not parser.read(config, encoding="utf-8"):
        raise RuntimeError(f"PCSX2 did not initialise its configuration: {config}")
    for section, values in settings.items():
        if not parser.has_section(section):
            parser.add_section(section)
        parser[section].update(values)
    with config.open("w", encoding="utf-8") as output:
        parser.write(output)
    return config


class Keyboard:
    """XTest key injection on the private Xvfb display."""

    def __init__(self, display: str) -> None:
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        self.xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x11.XStringToKeysym.restype = ctypes.c_ulong
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XKeysymToKeycode.restype = ctypes.c_uint
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.connection = None
        deadline = time.monotonic() + 15
        while not self.connection and time.monotonic() < deadline:  # Xvfb can report its number a moment early
            self.connection = self.x11.XOpenDisplay(display.encode())
            if not self.connection:
                time.sleep(0.5)
        if not self.connection:
            raise RuntimeError(f"cannot connect to the private display {display}")

    def keycode(self, name: str) -> int:
        code = self.x11.XKeysymToKeycode(self.connection, self.x11.XStringToKeysym(name.encode()))
        if not code:
            raise ValueError(f"unknown X11 keysym: {name}")
        return code

    def down(self, name: str) -> None:
        self.xtst.XTestFakeKeyEvent(self.connection, self.keycode(name), 1, 0)
        self.x11.XFlush(self.connection)

    def up(self, name: str) -> None:
        self.xtst.XTestFakeKeyEvent(self.connection, self.keycode(name), 0, 0)
        self.x11.XFlush(self.connection)

    def close(self) -> None:
        if self.connection:
            self.x11.XCloseDisplay(self.connection)
            self.connection = None


def stop_process(process: subprocess.Popen | None, grace: float = 30) -> None:
    """SIGTERM the process group, then SIGKILL it; never raises (a loaded host can make PCSX2 slow to exit)."""
    if process is None or process.poll() is not None:
        return
    for sig, timeout in ((signal.SIGTERM, grace), (signal.SIGKILL, 10)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=timeout)
            return
        except subprocess.TimeoutExpired:
            continue


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def settled(path: Path, quiet: float = 1.0, timeout: float = 60) -> bool:
    """True once the file exists and its size has stopped changing for `quiet` seconds."""
    deadline = time.monotonic() + timeout
    last = -1
    stable_since = None
    while time.monotonic() < deadline:
        size = path.stat().st_size if path.exists() else -1
        if size >= 0 and size == last:
            if stable_since is None:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= quiet:
                return True
        else:
            stable_since = None
        last = size
        time.sleep(0.25)
    return False


class Run:
    def __init__(self, args: argparse.Namespace, env: dict[str, str]) -> None:
        self.args = args
        self.iso = Path(env["DQ8_ISO"]).resolve(strict=True)
        self.bios = Path(env["DQ8_PS2_BIOS"]).resolve(strict=True)
        self.executable = prepare_emulator(env)
        self.profile = (Path(args.profile) if args.profile else WORK / "profile").resolve()
        self.profile.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        self.run_dir = WORK / f"{args.scenario}-{stamp}"
        self.run_dir.mkdir(parents=True)
        self.data = self.profile / "PCSX2"
        self.snaps = self.data / "snaps"
        self.sstates = self.data / "sstates"
        self.videos = self.data / "videos"
        self.logfile = self.run_dir / "emulog.txt"
        self.environment = dict(os.environ, QT_QPA_PLATFORM="xcb", XDG_CONFIG_HOME=str(self.run_dir / "xdg-config"),
                                XDG_CACHE_HOME=str(self.run_dir / "xdg-cache"), XDG_DATA_HOME=str(self.run_dir / "xdg-data"))
        self.report: dict = {
            "status": "failed", "scenario": args.scenario, "run_directory": str(self.run_dir),
            "profile": str(self.profile), "iso": str(self.iso), "iso_size": self.iso.stat().st_size,
            "bios_sha256": sha256(self.bios), "resume_state": str(args.resume) if args.resume else None,
            "screen": SCREEN, "steps": [], "marks": {}, "screenshots": [], "pcsx2_screenshots": [],
            "gs_dumps": [], "save_states": [], "videos": [], "input_replay": [], "emulog_excerpts": [],
            "crtc": [], "errors": [],
        }
        self.display_process: subprocess.Popen | None = None
        self.emulator: subprocess.Popen | None = None
        self.keyboard: Keyboard | None = None
        self.started = 0.0
        self.display = ""

    # ---- infrastructure -------------------------------------------------------------------------------------
    def elapsed(self) -> float:
        return round(time.monotonic() - self.started, 3)

    def start_display(self, log) -> None:
        read_fd, write_fd = os.pipe()
        try:
            self.display_process = subprocess.Popen(
                ["Xvfb", "-displayfd", str(write_fd), "-screen", "0", f"{SCREEN}x24", "-nolisten", "tcp"],
                pass_fds=(write_fd,), stdout=log, stderr=log, start_new_session=True)
            os.close(write_fd)
            if not select.select([read_fd], [], [], 15)[0]:
                raise RuntimeError("Xvfb did not report a display (see xvfb.log); the sandbox may block its socket")
            number = os.read(read_fd, 32).decode().strip()
        finally:
            os.close(read_fd)
        if not number.isdigit():
            raise RuntimeError("Xvfb failed to allocate a private display")
        self.display = f":{number}"
        self.environment["DISPLAY"] = self.display
        self.report["display"] = self.display
        self.keyboard = Keyboard(self.display)

    def pcsx2(self, *extra: str) -> list[str]:
        return [str(self.executable), "-datapath", str(self.profile), *extra]

    def check_version(self, log) -> None:
        version = subprocess.run(self.pcsx2("-version"), env=self.environment, timeout=30, capture_output=True, text=True)
        text = (version.stdout + version.stderr).strip()
        self.report["emulator_version"] = text
        if "PCSX2 v" not in text:
            raise RuntimeError(f"could not query the PCSX2 version: {text[:200]}")
        subprocess.run(self.pcsx2("-testconfig"), env=self.environment, check=True, timeout=30, stdout=log, stderr=log)
        settings = profile_settings(self.bios, self.args.verbose_cdvd)
        self.report["settings"] = settings
        self.report["ini"] = str(configure_profile(self.profile, settings))

    def launch(self) -> None:
        command = self.pcsx2("-nogui", "-fastboot", "-fullscreen", "-logfile", str(self.logfile))
        if self.args.resume:
            command += ["-statefile", str(Path(self.args.resume).resolve(strict=True))]
        command += ["--", str(self.iso)]
        self.report["command"] = command
        self.emulator = subprocess.Popen(command, env=self.environment, stdout=self.process_log, stderr=self.process_log,
                                         start_new_session=True)
        self.started = time.monotonic()

    def wait_for_game(self, timeout: float = 600) -> None:
        """Block until the guest is running: the ELF entry on a fresh boot, the GS thaw on a state resume."""
        marker = r"Loading GS" if self.args.resume else r"is executing"
        self.wait_for_log(marker, timeout)
        self.report["marks"]["game-running"] = self.elapsed()
        if not self.args.resume:
            self.wait_for_log(r"Set GS CRTC configuration", 300)
            self.report["marks"]["first-crtc"] = self.elapsed()
        self.wait(10 if self.args.resume else 1)

    def wait_for_log(self, pattern: str, timeout: float) -> str:
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.alive()
            text = self.logfile.read_text(errors="replace") if self.logfile.exists() else ""
            match = regex.search(text)
            if match:
                return text[match.start():text.find("\n", match.start())][:200]
            time.sleep(0.5)
        raise RuntimeError(f"emulog did not match {pattern!r} within {timeout}s")

    def alive(self) -> None:
        if self.emulator is not None and self.emulator.poll() is not None:
            raise RuntimeError(f"PCSX2 exited during the run (status {self.emulator.returncode})")

    def wait(self, seconds: float) -> None:
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            self.alive()
            time.sleep(min(0.2, max(0.0, until - time.monotonic())))

    def listing(self, folder: Path) -> set[Path]:
        return {p for p in folder.glob("*") if p.is_file()} if folder.is_dir() else set()

    def collect(self, folder: Path, before: set[Path], destination: Path, timeout: float = 120) -> Path:
        """Wait for one new file with destination's suffix in `folder`, let it finish writing, then move it.

        A GS dump also writes a PNG screenshot beside the .gs, so the suffix filter is what keeps the two apart.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.alive()
            new = sorted((p for p in self.listing(folder) - before if p.suffix == destination.suffix),
                         key=lambda p: p.stat().st_mtime)
            if new:
                source = new[-1]
                if not settled(source, quiet=1.5, timeout=timeout):
                    raise RuntimeError(f"{source.name} kept growing for {timeout}s")
                shutil.move(str(source), destination)
                for stray in self.listing(folder) - before:  # the dump's own PNG, backup states, ...
                    if stray.suffix != destination.suffix:
                        stray.unlink(missing_ok=True)
                return destination
            time.sleep(0.5)
        raise RuntimeError(f"no new {destination.suffix} file appeared in {folder} within {timeout}s")

    # ---- steps ----------------------------------------------------------------------------------------------
    def press(self, key: str, hold: float = DEFAULT_HOLD) -> None:
        assert self.keyboard is not None
        self.report["input_replay"].append({"t": self.elapsed(), "key": key, "hold": hold})
        self.keyboard.down(key)
        try:
            self.wait(hold)
        finally:
            self.keyboard.up(key)
        self.wait(0.35)

    def shot(self, name: str) -> dict:
        path = self.run_dir / f"{name}.png"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "x11grab", "-video_size", SCREEN, "-i",
                        self.display, "-frames:v", "1", "-y", str(path)], env=self.environment, check=True, timeout=30)
        entry = {"name": name, "t": self.elapsed(), "path": str(path), "sha256": sha256(path)}
        if self.args.ocr and shutil.which("tesseract"):
            entry["osd_ocr"] = self.ocr(path)
        self.report["screenshots"].append(entry)
        return entry

    def ocr(self, image: Path) -> list[str]:
        """OCR of the top band of the screen, where PCSX2 draws its performance OSD. Keeps only OSD-looking lines."""
        band = image.with_suffix(".osd.png")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(image), "-vf",
                        "crop=iw:ih*0.3:0:0,scale=iw*2:ih*2", "-y", str(band)], check=True, timeout=30)
        result = subprocess.run(["tesseract", str(band), "-", "--psm", "11"], capture_output=True, text=True, timeout=60)
        band.unlink(missing_ok=True)
        keep = re.compile(r"\d+\s*%|\bG:|\bV:|FPS|VPS|\d+x\d+|\bFT\b|frame|draw|prim|\bGS\b|readback|interl|progress|field",
                          re.IGNORECASE)
        return [line.strip() for line in result.stdout.splitlines() if keep.search(line)]

    def hotkey_file(self, hotkey: str, folder: Path, destination: Path, settle_before: float = 2.0) -> Path:
        before = self.listing(folder)
        self.press(HOTKEYS[hotkey])
        self.wait(settle_before)
        return self.collect(folder, before, destination)

    def run_step(self, step: str) -> None:
        verb, _, rest = step.partition(":")
        parts = rest.split(":") if rest else []
        record = {"t": self.elapsed(), "step": step}
        self.report["steps"].append(record)
        if verb == "wait":
            self.wait(float(parts[0]))
        elif verb == "press":
            self.press(parts[0], float(parts[1]) if len(parts) > 1 else DEFAULT_HOLD)
        elif verb == "mash":
            key, seconds = parts[0], float(parts[1])
            interval = float(parts[2]) if len(parts) > 2 else 1.0
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                self.press(key)
                self.wait(max(0.0, interval - 0.5))
        elif verb == "shot":
            record["result"] = self.shot(parts[0])["sha256"]
        elif verb == "probe":
            name, seconds, interval = parts[0], float(parts[1]), float(parts[2])
            until = time.monotonic() + seconds
            index = 0
            while time.monotonic() < until:
                self.shot(f"{name}-{index:03d}")
                index += 1
                self.wait(interval)
        elif verb == "snap":
            path = self.hotkey_file("Screenshot", self.snaps, self.run_dir / f"{parts[0]}.png", settle_before=1.0)
            self.report["pcsx2_screenshots"].append({"name": parts[0], "t": self.elapsed(), "path": str(path),
                                                     "sha256": sha256(path)})
        elif verb == "dump":
            path = self.hotkey_file("GSDumpSingleFrame", self.snaps, self.run_dir / f"{parts[0]}.gs", settle_before=3.0)
            self.report["gs_dumps"].append({"name": parts[0], "t": self.elapsed(), "path": str(path),
                                            "frames": 1, "size": path.stat().st_size})
        elif verb == "mdump":
            # PCSX2 2.7.524 records a fixed-length multi-frame dump (26 vsyncs, about 0.43 s) per hotkey press;
            # a second press would start another dump, so press once and wait S seconds for it to be written.
            before = self.listing(self.snaps)
            self.press(HOTKEYS["GSDumpMultiFrame"])
            self.wait(float(parts[1]) if len(parts) > 1 else 3.0)
            path = self.collect(self.snaps, before, self.run_dir / f"{parts[0]}.gs", timeout=600)
            self.report["gs_dumps"].append({"name": parts[0], "t": self.elapsed(), "path": str(path),
                                            "size": path.stat().st_size})
        elif verb == "state":
            path = self.hotkey_file("SaveStateToSlot", self.sstates, self.run_dir / f"{parts[0]}.p2s", settle_before=3.0)
            self.report["save_states"].append({"name": parts[0], "t": self.elapsed(), "path": str(path),
                                               "size": path.stat().st_size})
        elif verb == "rec":
            before = self.listing(self.videos)
            self.press(HOTKEYS["ToggleVideoCapture"])
            self.wait(float(parts[1]))
            self.press(HOTKEYS["ToggleVideoCapture"])
            self.wait(3.0)
            path = self.collect(self.videos, before, self.run_dir / f"{parts[0]}.mp4", timeout=300)
            self.report["videos"].append({"name": parts[0], "t": self.elapsed(), "path": str(path),
                                          "size": path.stat().st_size})
        elif verb == "mark":
            self.report["marks"][parts[0]] = self.elapsed()
        elif verb == "waitlog":
            record["result"] = self.wait_for_log(parts[0], float(parts[1]))
        else:
            raise ValueError(f"unknown step: {step}")

    # ---- driver ----------------------------------------------------------------------------------------------
    def harvest_log(self) -> None:
        if not self.logfile.exists():
            return
        lines = self.logfile.read_text(errors="replace").splitlines()
        self.report["emulog_excerpts"] = [l for l in lines if LOG_EXCERPT.search(l)][:400]
        self.report["crtc"] = [l for l in lines if "Set GS CRTC configuration" in l]
        self.report["emulog_lines"] = len(lines)

    def execute(self, steps: list[str]) -> dict:
        xvfb_log = self.run_dir / "xvfb.log"
        with (self.run_dir / "process.log").open("wb") as self.process_log, xvfb_log.open("wb") as display_log:
            try:
                for tool in ("Xvfb", "ffmpeg"):
                    if shutil.which(tool) is None:
                        raise FileNotFoundError(f"required tool not installed: {tool}")
                self.start_display(display_log)
                self.check_version(self.process_log)
                self.launch()
                self.wait_for_game()
                for step in steps:
                    self.run_step(step)
                self.report["status"] = "recorded"
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                self.report["errors"].append(str(error))
                try:
                    if self.emulator is not None and self.emulator.poll() is None:
                        self.shot("on-error")
                except Exception as late:  # noqa: BLE001 - best effort
                    self.report["errors"].append(f"on-error screenshot failed: {late}")
            finally:
                if self.keyboard is not None:
                    self.keyboard.close()
                self.report["run_seconds"] = self.elapsed() if self.started else None
                stop_process(self.emulator)
                stop_process(self.display_process, grace=5)
                for process in (self.emulator, self.display_process):
                    if process is not None and process.poll() is None:
                        self.report["errors"].append(f"process {process.pid} did not exit; kill it by hand")
        self.harvest_log()
        report_path = self.run_dir / "report.json"
        report_path.write_text(json.dumps(self.report, indent=2) + "\n", encoding="utf-8")
        self.report["report_path"] = str(report_path)
        return self.report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scenario", choices=sorted(SCENARIOS), default="probe")
    parser.add_argument("--resume", type=Path, help="save state (.p2s) to load at start (PCSX2 -statefile)")
    parser.add_argument("--step", action="append", default=[], help="extra step; replaces the scenario's steps for probe")
    parser.add_argument("--replace-steps", action="store_true", help="run only the --step list, ignoring the scenario's")
    parser.add_argument("--env", type=Path, default=LOCAL_ENV, help="tools/local.env to read paths from")
    parser.add_argument("--profile", type=Path, help="private PCSX2 data path (default work/pcsx2/profile)")
    parser.add_argument("--ocr", action="store_true", help="OCR the OSD band of every screenshot with tesseract")
    parser.add_argument("--verbose-cdvd", action="store_true", help="log every CDVD read (sector numbers) to emulog.txt")
    args = parser.parse_args(argv)

    env = load_local_env(args.env)
    missing = [k for k in ("DQ8_ISO", "DQ8_PS2_BIOS") if not env.get(k)]
    if missing:
        print(f"missing in {args.env}: {', '.join(missing)}", file=sys.stderr)
        return 2
    steps = list(args.step) if (args.replace_steps or args.scenario == "probe") else SCENARIOS[args.scenario] + list(args.step)
    report = Run(args, env).execute(steps)
    summary = {k: report[k] for k in ("status", "scenario", "run_directory", "run_seconds", "marks", "errors")}
    summary["screenshots"] = len(report["screenshots"])
    summary["gs_dumps"] = [d["path"] for d in report["gs_dumps"]]
    summary["save_states"] = [s["path"] for s in report["save_states"]]
    summary["crtc"] = report["crtc"][-3:]
    print(json.dumps(summary, indent=2))
    return 0 if report["status"] == "recorded" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
