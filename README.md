# DQ8-Native (working title)

An open-source **native PC port** of *Dragon Quest VIII: Journey of the Cursed King* (PlayStation 2, NTSC-U),
built by statically recompiling the game's own code to C++ and running it on a purpose-built runtime.
It is **not an emulator**: the game's code is compiled for your PC, and a launcher sets it up, configures it and
starts it.

> **Status: pre-alpha.** The game boots, plays its movies and sound, draws the field in 3D, opens the menu, and saves
> and loads. It is not ready for normal play yet:
> - **Speed:** the title and menus run at full speed, the 3D field at about a quarter of it (milestone M6 speeds it up).
> - **Coverage:** towns, shops and battles have not been verified yet.
> - **Known glitches:** missing dots in the menu's separators, two lighting glows that are too bright.
> - **Launcher:** new in M5; its first-run setup has not yet been tried end to end on a real disc.
> - **Windows:** builds and passes its tests under Wine in CI, but has not run on real Windows yet.
>
> Progress is recorded per milestone in [Docs/milestones/](Docs/milestones/M5-launcher.md).

## You bring your own game

This project contains **no game code, no game data and no BIOS**. To play, you need your own legally obtained copy
of the game as a disc image (`.iso`, NTSC-U, `SLUS_212.07`). The setup verifies your image, then converts and builds
everything on your machine. Nothing derived from the game is ever committed to this repository or distributed in
releases. See [Docs/project-brief.md](Docs/project-brief.md) §9.

## Building from source (Linux)

There are no release downloads yet; CI builds a Linux AppImage and a Windows zip on every push to `main`.

You need:
- GCC 11 or Clang 14 or newer, CMake 3.25 or newer, Ninja and Git.
- A GPU with Vulkan drivers (the paraLLEl-GS renderer).
- The development headers SDL3 builds against (X11, Wayland, ALSA, PulseAudio, ...; `SDL3_LINUX_DEPS` in
  [.github/workflows/ci.yml](.github/workflows/ci.yml) lists them for Ubuntu). Alternatively, configure with
  `-DDQ8_USE_SYSTEM_SDL3=ON` to use an installed SDL 3.4.
- Optional: `libavcodec` and `libavutil` development files, for the movies. Without them the movies are skipped.

```bash
git clone https://github.com/Igrise12/DQVIIIPC.git
```

```bash
cd DQVIIIPC && bash scripts/init_submodules.sh
```

```bash
cmake --preset release -DDQ8_BUILD_RECOMP=ON
```

```bash
cmake --build --preset release --parallel
```

```bash
cmake --install build/release --prefix "$HOME/.local/opt/dq8-native"
```

`DQ8_BUILD_RECOMP=ON` also builds the recompiler (`ps2_recomp`), which the setup needs. The install holds
`bin/` (`dq8-launcher`, `dq8-game`, `dq8ctl`, `ps2_recomp`), `lib/` and `share/dq8/`, and can be moved anywhere.

## Playing

Start the launcher:

```bash
"$HOME/.local/opt/dq8-native/bin/dq8-launcher"
```

1. **Setup:** choose your disc image. The launcher checks it and looks for the compiler, CMake and Ninja.
2. **Start setup:** the game's code is recompiled and built on your machine, about 10 minutes the first time, with
   progress per stage. You can cancel and resume.
3. **Play** starts the game once the setup has finished.
4. **Settings** (window mode, internal resolution 1x–4x, aspect ratio including the game's own 16:9 option, volume,
   ...) and **Controllers** (key and gamepad bindings per device) apply the next time you press Play.
5. **Diagnostics** exports a support report for bug reports. It never contains anything from the game.

Default controls: arrow keys for the D-pad, `K` Cross, `L` Circle, `J` Square, `I` Triangle, `Return` Start,
`Backspace` Select, `W` `A` `S` `D` for the left stick. Gamepads work by button position (the bottom face button is
Cross).

Where things go:
- the setup's build and the list of installed games: `~/.local/share/dq8-native/`
- settings and controller profiles: `~/.config/dq8-native/`
- memory cards (saves): `~/.local/share/dq8-native/`

**Without the launcher.** `dq8ctl setup <your.iso>` runs the same setup from a terminal. `dq8-game --iso <your.iso>
--module-dir <modules> --window` runs the game directly, and `dq8-game --help` lists its options.

## Windows

The Windows build is cross-compiled from Linux with llvm-mingw
([ADR-0018](Docs/adr/0018-windows-llvm-mingw.md)); on first setup it asks before downloading the matching compiler.
See [packaging/windows/README.md](packaging/windows/README.md). It has not yet been tried on real Windows.

## License

The project's own code is licensed under the **GNU General Public License, version 3 or (at your option) any later
version** (GPL-3.0-or-later). See [LICENSE](LICENSE). Third-party components keep their own licenses and are listed
in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The license covers this project's code only. It grants no rights
to any game content.

*Dragon Quest* is a trademark of Square Enix. This project is unofficial and is not affiliated with or endorsed by
Square Enix, Level-5 or Sony.

## Documentation

| Document | Contents |
|---|---|
| [Docs/project-brief.md](Docs/project-brief.md) | Vision, goals, scope, facts about the game, features, legal stance, milestones, risks |
| [Docs/architecture.md](Docs/architecture.md) | Setup pipeline, runtime, renderer, launcher, extension points, decision log |
| [Docs/tech-stack.md](Docs/tech-stack.md) | Languages, build system, dependencies and licenses, tooling, platforms |
| [Docs/milestones/](Docs/milestones/M5-launcher.md) | One record per milestone: plan, contracts, results (M2 onward) |
| [Docs/adr/](Docs/adr/0018-windows-llvm-mingw.md) | Architecture decision records |
| [Docs/formats/](Docs/formats/function-map.md) | Our own file formats and the game's file layouts (no game data) |
| [CLAUDE.md](CLAUDE.md) | Contributor commands, hard rules and layering in one page |

## Workspace layout

```
Docs/         Project documentation
src/          Our own code: core, runtime, game layer, dq8-game (host), launcher, CLI
include/      Public headers, and the ABI the recompiled game modules compile against
tools/        Contributor tools: Ghidra scripts, map publishing, bring-up helpers
config/       Authored configuration (the settings schema and its texts)
data/         Region profiles, published function maps, smoke-test inputs (never game bytes)
cmake/        Build helpers, the game-module template, toolchain files
third_party/  Pinned dependencies (git submodules; see THIRD_PARTY_NOTICES.md)
tests/        Unit, local (your disc) and end-to-end tests; fixtures contain no game data
packaging/    Linux AppImage and Windows zip
scripts/      Developer helpers and the no-game-data checker
Game/         LOCAL ONLY, git-ignored: your disc image lives here
```

Generated output (`baserom/`, `generated/`, `work/`, `build/`) is created locally and git-ignored.
