# DQ8-Native (working title)

An open-source **native PC port** of *Dragon Quest VIII: Journey of the Cursed King* (PlayStation 2, NTSC-U),
built by statically recompiling the game's own code and running it on a purpose-built runtime.
It is **not an emulator**, and it ships with a launcher, resolution scaling and controller configuration.

> **Status: pre-alpha, planning stage.** There is no playable build and no source code yet.
> This repository currently holds the project documentation and the workspace layout.

## You bring your own game

This project contains **no game code, no game data and no BIOS**. To play, you need your own legally
obtained copy of the game as a disc image. The launcher verifies your image, then converts and builds
everything on your machine. Nothing derived from the game is ever committed to this repository or
distributed in releases. See [Docs/project-brief.md](Docs/project-brief.md) §9.

## License

The project's own code is licensed under the **GNU General Public License, version 3 or (at your option) any later
version** (GPL-3.0-or-later). See [LICENSE](LICENSE). Third-party components keep their own licenses and are listed in
`THIRD_PARTY_NOTICES.md` once the first dependency is added. The license covers this project's code only. It grants no
rights to any game content.

*Dragon Quest* is a trademark of Square Enix. This project is unofficial and is not affiliated with or
endorsed by Square Enix, Level-5 or Sony.

## Documentation

| Document | Contents |
|---|---|
| [Docs/project-brief.md](Docs/project-brief.md) | Vision, goals, scope, target game facts, features, legal stance, milestones, risks |
| [Docs/architecture.md](Docs/architecture.md) | Setup pipeline, runtime layers, launcher, extension points, testing |
| [Docs/tech-stack.md](Docs/tech-stack.md) | Languages, build system, dependencies, tooling, platform support |

## Workspace layout

```
Docs/         Project documentation
src/          Our own code (core, runtime, game glue, features, host exe, launcher, CLI)
include/      Public headers shared between the runtime, features and generated code
tools/        Offline tools: disc extraction, Ghidra scripts, recompiler driver, VU1 compiler
config/       Authored configuration (recompiler templates, function maps, settings schema)
data/         Shape-only tables and region profiles (never game bytes)
patches/      Patch queue applied to pinned third-party sources
third_party/  Pinned dependencies (git submodules, added later)
tests/        Unit tests, oracle (differential) tests, fixtures that contain no game data
packaging/    Release packaging
scripts/      Setup and developer helper scripts
Game/         LOCAL ONLY, git-ignored: your disc image lives here
```

Generated output (`baserom/`, `generated/`, `build/`) is created locally and git-ignored.
