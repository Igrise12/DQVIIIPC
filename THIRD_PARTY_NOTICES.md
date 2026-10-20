# Third-party notices

Every dependency this project bundles or links, with its license. Update this file in the same commit that adds
or bumps a dependency (tech-stack.md §10). Licenses of bundled code are reproduced in each submodule's own tree.

## Bundled (git submodules under `third_party/`)

| Component | Version | Commit | License | Used for |
|---|---|---|---|---|
| [toml11](https://github.com/ToruNiina/toml11) | v4.4.0 | `be08ba2be2a964edcdb3d3e3ea8d100abc26f286` | BSL-1.0 | Configuration, region profiles, settings |
| [Catch2](https://github.com/catchorg/Catch2) | v3.16.1 | `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | Unit tests (not shipped in releases) |
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | eb8688b | `eb8688b2fd191129f103a39543002c005b33409c` | GPL-3.0 | The recompiler (`ps2_recomp`), built from source and shipped with the host (ADR-0003); `include/dq8/abi/ps2recomp/` adapts its context and macro headers (GPL-3.0-only, attributed) |
| [ELFIO](https://github.com/serge1/ELFIO) | Release_3.12 | `8ae6cec5d60495822ecd57d736f66149da9b1830` | MIT | ELF reader inside `ps2_recomp` |
| [fmt](https://github.com/fmtlib/fmt) | 12.1.0 | `407c905e45ad75fc29bf0f9bb7c5c2fd3475976f` | MIT | Formatting inside `ps2_recomp` |
| [libdwarf](https://github.com/davea42/libdwarf-code) | v2.2.0 | `2e088b983562860f497b4fd789a3e61524eb857c` | LGPL-2.1 (library) | DWARF reader inside `ps2_recomp` |
| [rabbitizer](https://github.com/Decompollaborate/rabbitizer) | 1.14.3 | `d5804fa488474830a47fbf9f3693048ec1e45589` | MIT | MIPS disassembler inside `ps2_recomp` |

## Tools used during development, not bundled or redistributed

| Tool | License | Role |
|---|---|---|
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) 12.1.4 | Apache-2.0 | Static analysis of the user's boot ELF (contributors only) |
| [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) 2.1.38 | Apache-2.0 | Emotion Engine (R5900) support for Ghidra |
| [REA](https://github.com/morluto/rea) 6.7.0 | MIT | Agent-facing MCP server and CLI over Ghidra |
| [PCSX2](https://github.com/PCSX2/pcsx2) 2.7.524 | GPL-3.0 | Reference emulator: behaviour ground truth, GS dumps, save states |
| [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) | LGPL-3.0-or-later | Candidate first GS backend (planned; pinned when milestone M3 starts) |

The game itself, its executable, data, movies, and the PS2 BIOS are the property of their rights holders and are
never part of this project. See project-brief.md §9.
