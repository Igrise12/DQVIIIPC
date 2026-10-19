# Third-party notices

Every dependency this project bundles or links, with its license. Update this file in the same commit that adds
or bumps a dependency (tech-stack.md §10). Licenses of bundled code are reproduced in each submodule's own tree.

## Bundled (git submodules under `third_party/`)

| Component | Version | Commit | License | Used for |
|---|---|---|---|---|
| [toml11](https://github.com/ToruNiina/toml11) | v4.4.0 | `be08ba2be2a964edcdb3d3e3ea8d100abc26f286` | BSL-1.0 | Configuration, region profiles, settings |
| [Catch2](https://github.com/catchorg/Catch2) | v3.16.1 | `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | Unit tests (not shipped in releases) |

## Tools used during development, not bundled or redistributed

| Tool | License | Role |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | Apache-2.0 | Static analysis of the user's boot ELF (contributors only) |
| [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) 2.1.38 | Apache-2.0 | Emotion Engine (R5900) support for Ghidra |
| [REA](https://github.com/morluto/rea) 6.7.0 | MIT | Agent-facing MCP server and CLI over Ghidra |
| [PCSX2](https://github.com/PCSX2/pcsx2) 2.7.524 | GPL-3.0 | Reference emulator: behaviour ground truth, GS dumps, save states |
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | GPL-3.0 | Static recompiler (planned dependency; pinned when milestone M2 starts) |
| [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) | LGPL-3.0-or-later | Candidate first GS backend (planned; pinned when milestone M3 starts) |

The game itself, its executable, data, movies, and the PS2 BIOS are the property of their rights holders and are
never part of this project. See project-brief.md §9.
