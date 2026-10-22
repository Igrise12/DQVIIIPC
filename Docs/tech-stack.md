# Tech Stack: DQ8-Native

| | |
|---|---|
| **Status** | Proposed. Choices are recommendations with reasons and alternatives. Versions are policy, not pins: exact pins are made when each dependency is added. |
| **Last updated** | 2026-10-19 |
| **Read first** | [project-brief.md](project-brief.md) for goals and constraints, [architecture.md](architecture.md) for how the pieces fit |

**Contents:** 1 At a glance · 2 Languages · 3 Build system · 4 Compilers and platforms · 5 Runtime dependencies · 6 Building the game module · 7 Analysis and reverse-engineering tools · 8 Testing and quality · 9 CI, packaging, release · 10 Licensing and dependency policy · 11 Alternatives considered · 12 Development environment · 13 Spikes that validate the stack

---

## 1. At a glance

| Concern | Choice | Why in one line |
|---|---|---|
| Core language | **C++20** | Required by PS2Recomp, generated code is C++, one language for runtime and tools |
| Module boundary | **Plain C ABI** | Lets the game module be built with a different compiler than the host, which makes a bundled toolchain possible |
| Recompiler | **PS2Recomp** (GPL-3.0), pinned plus patch queue | The only general PS2 recompiler with an active ecosystem |
| Build | **CMake + Ninja**, presets, git submodules for dependencies | Matches PS2Recomp and its dependencies, reproducible and offline-friendly |
| Windowing, input, audio | **SDL3** | One portable library for window, gamepad, haptics and audio |
| Graphics API | **Vulkan 1.3+** | Required by the first GS backend, portable across Windows and Linux |
| First GS backend | **paraLLEl-GS** (LGPL-3.0-or-later), shared library | Accurate and already supports 2x to 16x supersampling |
| Launcher UI | **Dear ImGui** over SDL3 (SDL's renderer backend for the launcher, ADR-0010), behind a front-end abstraction | Shares a toolkit with the in-game overlay and adds no runtime |
| Config | **TOML** (JSON-lines for process events) | Human-editable, comments allowed, PS2Recomp already uses it |
| Tests | **Catch2 + CTest**, sanitizers, libFuzzer | Standard, CI-friendly |
| CI | **GitHub Actions**: Linux GCC and Clang, Windows MSVC | Free for public repositories |
| Reverse engineering | **Ghidra** with an Emotion Engine plugin, **PCSX2**, **ImHex** | The same tools sibling projects use |
| License | **GPL-3.0-or-later** (decided) | Follows from linking PS2Recomp (brief §9) |

---

## 2. Languages

| Language | Used for | Notes |
|---|---|---|
| **C++20** | Runtime, core, launcher, CLI, offline tools, generated game module | Result types for errors in hot paths. Exceptions are allowed in the launcher and tools, but the runtime's per-instruction and per-packet paths are exception-free. Generated code builds without exceptions and RTTI. |
| **C (C11)** | The host and module ABI header `include/dq8/abi/game_module.h` | No STL or C++ types cross the boundary; exceptions do until M3 (architecture.md §13.3). |
| **Python 3** | Developer scripts and analysis glue | **Never on the end user's setup path**, so users need no Python. Scripts that read game files run with `python -I` and from a directory that holds no untrusted files. |
| **Java** | Ghidra scripts (`tools/ghidra/`) | Developers only. Needs the JDK that the chosen Ghidra release requires. |
| **GLSL** | Shaders for the native renderer, presentation passes, overlay | Compiled to SPIR-V at build time. |
| **TOML, CSV, Markdown** | Configuration and region profiles (TOML), function maps (CSV), documents (Markdown with Mermaid diagrams) | |

**Rule of thumb:** anything a player runs is C++ and ships in the release. Python and Java are contributor tools.

---

## 3. Build system and project tooling

| Item | Choice | Notes |
|---|---|---|
| Build generator | **CMake**, minimum 3.25, with **Ninja** | PS2Recomp needs 3.20 or newer. We set 3.25 for current preset features. |
| Presets | `CMakePresets.json` with dev presets per platform and compiler and a release preset | Contributors run one command. |
| Dependencies | **Git submodules in `third_party/`**, pinned to commits | Reproducible and works offline. `DQ8_USE_SYSTEM_<LIB>` options let distribution packagers substitute system libraries. Third-party projects that use FetchContent internally (PS2Recomp) get `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides pointing at our submodules. |
| Patch queue | `patches/` applied in a fixed order to pinned third-party sources | The same approach other PS2 ports use for PS2Recomp. Keep patches small and upstream them. |
| Compiler cache | **ccache** (Linux), **sccache** (Windows), keyed on file contents | Essential for the game-module build (§6). |
| Shader compilation | `glslc` or `glslangValidator` from the Vulkan SDK during the build | If this proves a contributor-friction point, commit pinned SPIR-V as a fallback, as one sibling project does. |
| Formatting and linting | `clang-format` (config in the repo), `clang-tidy` with a curated check set | Enforced in CI and as a pre-commit step. |
| Pre-commit | The `pre-commit` framework running formatting, whitespace, and the **no-game-data check** | Architecture.md §14. |
| Documentation | Markdown in `Docs/`, ADRs in `Docs/adr/`, Mermaid for diagrams | A generated site (for example MkDocs) is optional later. |

**Out of scope for now:** vcpkg and Conan (adds a second dependency mechanism). We can add a vcpkg manifest later if Windows contributors need it.

---

## 4. Compilers and platforms

### 4.1 Toolchains

| Platform | Compilers | Notes |
|---|---|---|
| **Linux x86-64** (Tier 1) | GCC 13 or newer, Clang 17 or newer | Both must work. |
| **Windows 10/11 x86-64** (Tier 1) | MSVC (Visual Studio 2022, recent update), clang-cl as an alternative | PS2Recomp reports its testing is mainly with MSVC. |
| **Linux ARM64** (Tier 2) | Same as Linux | Needs NEON equivalents of the SSE paths. After 1.0. |
| **macOS Apple Silicon** (Tier 3) | Apple Clang | Blocked on a Vulkan layer that meets the parallel-gs feature list. Unscheduled. |

The development machine runs very new compilers (see §12). CI must **also** build with an LTS-distribution
toolchain (Ubuntu 24.04's GCC and Clang) so the stated minimums stay honest.

### 4.2 CPU and GPU requirements

| Requirement | Detail |
|---|---|
| **CPU** | x86-64 with **SSE4.1** as a baseline. PS2Recomp notes SSE4 and AVX host support for some vector paths. Decide whether AVX is optional once helpers are written. |
| **GPU API** | **Vulkan 1.3** capable device on Windows and Linux |
| **Features needed by parallel-gs** | Documented: `descriptorIndexing`, `timelineSemaphore`, `storageBuffer8BitAccess`, `storageBuffer16BitAccess`, `shaderInt16`, `scalarBlockLayout`, subgroup operations (basic, vote, arithmetic, ballot, shuffle; clustered is not needed), subgroup size control with full subgroups for sizes 4–64 (16–64 only for 16x super-sampling). Checked by the code but undocumented: `bufferDeviceAddress` and at least 32 KiB of compute shared memory. All verified on the RTX 4060 on 2026-10-19 (spike S5a); measured on this game's frames (S5b): 5 ms of GPU per field frame at 1x, 18.5 ms at 16x, 576–720 MiB of device memory. Reported as fine on current desktop drivers; tested by its author on RADV, AMDVLK, amdgpu-pro, Steam Deck, NVIDIA RTX and Intel Arc. |
| **Reference performance machine** | 6 cores, RTX 4060 class GPU, 16 GB RAM |

The launcher's Diagnostics page reports whether the current device satisfies the active backend's requirements (architecture.md §11.2).

---

## 5. Runtime dependencies

Versions: use the **latest stable release at the time each dependency is added** and pin by commit. License identifiers are what each project states and are verified again when the dependency is added.

| Role | Dependency | License | Notes |
|---|---|---|---|
| **Window, input, gamepad, haptics, audio** | **SDL3** | zlib | Replaces separate libraries for each. Gamepad mapping database included. |
| **Vulkan loader** | **volk** | MIT | No link-time dependency on the Vulkan loader. |
| **Vulkan headers** | Vulkan-Headers | Apache-2.0 or MIT | |
| **GPU memory allocation** | Vulkan Memory Allocator | MIT | For the native renderer and presentation passes. |
| **GS backend, first** | **paraLLEl-GS** `cc6184a` (2026-10-06) with Granite `16e7395` | LGPL-3.0-or-later (Granite: MIT) | **Shared library**, so the LGPL relinking terms hold: one `dq8-gs-parallel` library containing our wrapper, parallel-gs and the four Granite archives it needs (`granite-vulkan`, `-util`, `-math`, `-volk`) with hidden visibility, exporting only our C ABI. Embedding API is the `GSInterface` class (about 25 methods, all calls from one thread); wrapper estimated at 1,000–1,500 lines (spike S5a). C++14, precompiled SPIR-V header, so the library build needs three submodules and no shader toolchain; the contributor-only tools (replayer, stream, repro) need ten more plus SDL3 and build the SPIR-V toolchain. The author currently does not accept outside contributions. |
| **ELF parsing** | ELFIO | MIT | Also used by PS2Recomp. |
| **Configuration** | toml11 | BSL-1.0 | Also used by PS2Recomp, so no second TOML library. |
| **JSON** | nlohmann/json v3.12.0 (submodule, M5) | MIT | Process events and small data files. |
| **Formatting and logging** | fmt, spdlog | MIT | fmt is also a PS2Recomp dependency. |
| **Launcher and overlay UI** | **Dear ImGui** v1.92.9 (submodule, M5; SDL3 platform and SDL renderer backends for the launcher, Vulkan for the overlay later) | MIT | One toolkit for launcher and in-game overlay. File pickers use SDL3's native dialogs. |
| **Upscaling shader** | AMD FidelityFX FSR 1 (EASU and RCAS) | MIT | Presentation pass (architecture.md §6.3). |
| **Fibers** | **minicoro** `02dad0f` (fallbacks behind `rt::Fiber`: ucontext, Win32 fibers) | MIT-0 or Unlicense | Guest threads (architecture.md §12.3, ADR-0007, accepted 2026-10-21). Annotates fiber switches for AddressSanitizer itself and saves the Windows thread-information block. Evaluate cost with deep guest stacks. |
| **Fast hashing** | xxHash | BSD-2-Clause | Cache keys, texture hashes. |
| **Digests** | A small vendored SHA-1 and CRC-32 implementation, or the platform crypto API | Public domain or system | Disc verification. |
| **Compression** | zstd | BSD-3-Clause | Caches and dumps. |
| **Image loading** | stb_image and stb_image_write | Public domain or MIT | Texture packs and screenshots. |
| **Movie decoding** | **FFmpeg libavcodec**, LGPL build, dynamically linked, `mpeg2video` decoder only (recommended by spikes S6a and S6b; ADR with the runtime design). Fallback: libmpeg2 | LGPL-2.1+; GPL-2.0+ | Behind `IMovieDecoder`, fed by an `sceMpeg`-level HLE (architecture.md §7.2): the game's own code decrypts and demuxes, so no libavformat, no demux library and no audio codec are needed; the decoder only handles MPEG-2 Main profile 512 × 448 at 30 fps (`Docs/formats/mvi.md`). `pl_mpeg` (MIT) is MPEG-1 only and unsuitable. |
| **Recompiler** | **PS2Recomp** (`ps2xRecomp`, `ps2xAnalyzer`, and its IOP executor) | GPL-3.0 | Pinned commit (`eb8688b`, 2026-10-14) plus patch queue. It has no git submodules: FetchContent pulls nine repositories at configure time (`ps2xRecomp`: ELFIO `Release_3.12`, toml11 `v4.4.0`, fmt `12.1.0`, libdwarf `v2.2.0`, rabbitizer `1.14.3`; `ps2xAnalyzer`: nlohmann/json `v3.11.3`; `ps2xRuntime`: raylib `5.5`, Dear ImGui `v1.92.7-docking`, rlImGui, plus system FFmpeg on Linux). Only the first six matter for the tools we keep. `FETCHCONTENT_FULLY_DISCONNECTED=ON` with `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides configures offline in 4.5 s (verified, S1). Builds unpatched with GCC 16 and Clang 23 (60–80 s, 445/445 tests); upstream CI already covers Linux GCC and Clang, so the README's "tested mainly with MSVC" is stale. |
| **Profiling** | Tracy (optional) | BSD-3-Clause | Development builds only. |
| **HTTP for update checks** | Platform HTTP API or libcurl, in the launcher only, optional | OS or curl license | Not needed until the opt-in update check is built. |

**Not chosen as dependencies but used as references:** PCSX2 (EE interpreter, VU and SPU2 behavior, GS dumps). Code taken from it must be attributed and is GPL-3.0-compatible with our license.

---

## 6. Building the game module

The game module is thousands of generated C++ files compiled on the **user's machine** (architecture.md §3, stages 5 and 6).
This is the least conventional part of the stack and the main usability risk, so it gets its own plan.

### 6.1 Characteristics

- Measured by spike S1 on the boot ELF with the analyzer's heuristic map (8,314 functions): the recompiler runs in 2 s and 0.6 GiB and writes 8,335 files, 201 MB, 4.77 M lines of C++ (deterministic: byte-identical across worker settings). A typical unit is 6 KB of source but preprocesses to 174 k lines, so it costs about 1.6 CPU-s with GCC 16 or Clang 23 at `-O1` or `-O2` alike; the 23 MB dispatch table (`register_functions.cpp`) costs 122 CPU-s and 2 GiB, the largest single function (7.2 MB) 204 CPU-s and 3 GiB. Whole tree: about 15,000–16,000 CPU-s, about 6,000 with a precompiled header; 30–40 minutes on six cores (≈15 with the PCH); objects about 125 MB.
- Our program has a boot ELF plus six overlays, each its own code unit; expect fewer and cleaner functions with the S2 map (9,598).

### 6.2 Build settings

| Setting | Choice | Reason |
|---|---|---|
| Chunking | One file per function (PS2Recomp offers only that or a single file) | Chunking N functions per file needs an emitter patch; with the precompiled header the per-file cost is already low (S1: 0.6 CPU-s per typical unit). |
| Optimization | `-O2` for all function units; `-O0` or `-O1` for the generated dispatch table | S1: `-O2` costs the same CPU as `-O1` on typical units (front end dominates), so the split buys nothing there; the table is a static initialiser with nothing to optimise. |
| Language flags | No exceptions, no RTTI (goal) | At `eb8688b` the emitter writes `throw` on unhandled instructions (422 units under the heuristic map), so this needs a clean function map or an emitter patch first (S1). |
| LTO | Off for the module | Link time grows sharply and benefits are small across a C ABI. |
| Precompiled header | The four runtime headers every unit includes | Measured 0.37× per unit (1.6 → 0.6 CPU-s, S1): the single largest saving; on by default. |
| Output | One shared library **per code unit** (main plus each overlay) | Parallel linking, and a change to one overlay's map rebuilds only that library. Required anyway: each recompiler run emits its own dispatch table under the same symbol names, so units cannot share one image unprefixed (S1). The few multi-megabyte single-function units set the critical path and should be split by the emitter or the map. |
| Cache | ccache or sccache, keyed on generated content | The recompiler is deterministic, so unchanged output is a cache hit. |
| Short work path on Windows | Build under a short directory such as `%LOCALAPPDATA%\dq8\w` | Another PS2 port hit the 260-character path limit. |

### 6.3 Where the compiler comes from

Because the host and module talk only through a C ABI, **the module's compiler does not have to match the host's**. That
allows three options:

| Option | How | Trade-off |
|---|---|---|
| **A. System compiler** | The launcher detects GCC, Clang or MSVC and shows instructions if none is found | No download, but the player must install build tools. Acceptable on Linux, harsh on Windows. |
| **B. Bundled toolchain** | The launcher downloads a pinned toolchain on demand. Candidates: an `llvm-mingw` style distribution on Windows (redistributable headers and libraries, which the MSVC runtime headers are not), and `zig cc` as a cross-platform clang driver | Best player experience. Needs a download step that the user approves, a verified checksum, and a license review of the bundle. |
| **C. Pre-built game module** | Not allowed: it would distribute recompiled game code (brief §9) | Rejected. |

**Proposal:** Linux defaults to A. Windows defaults to B. Both are selectable. The choice is validated in spike S1 and revisited at M5.

---

## 7. Analysis and reverse-engineering tools

These are for **contributors**. Players do not need any of them, because the function maps ship in the repository.

| Tool | Use | License | Notes |
|---|---|---|---|
| **Ghidra** 12.1.4 | Disassembly, function discovery, the PS2Recomp export script (`ExportPS2Functions.java`) | Apache-2.0 | With the `ghidra-emotionengine-reloaded` extension (2.1.38, language `r5900:LE:32:default`). Needs JDK 21 or newer; JDK 27 is accepted. The export script is interactive (`askFile`), so headless runs need a `.properties` answer file; spike S2 confirmed it runs unmodified that way. Headless import and analysis of the boot ELF take 2–3 minutes and 3.7 GB RSS (JDK 27). First-party scripts in `tools/ghidra/`: `headless.sh`, `Dq8AnalysisOptions.java`, `MapElfSegmentTails.java` (maps the bss and overlay window that the ELF loader skips), `InstructionCensus.java`, `ExportPS2Functions.properties`, `QueryRefs.java` (string, reference, constant, caller and decompile queries in one read-only run; S4), `MovieXrefs.java` (S6b) and `LoadOverlay.java` (loads a `BIN` overlay into a project copy at its load address and seeds code in four passes, 4–18 s per overlay; S3). The exporter's CSV lists labels as well as functions (68,791 records for 9,718 functions) and must be filtered before use. |
| **REA** (`rea-agents` 6.7.0) | MCP server and CLI over the Ghidra install, for interactive queries from agent sessions (functions, xrefs, bytes, decompile) | MIT | Spike S0 (2026-10-19): it **refuses our R5900 ELF** at its own admission step (`architecture_unsupported`, MIPS32/32R2 only, no PS2 profile), with or without the language override, so all analysis of the game runs on headless Ghidra (`tools/ghidra/`). REA stays registered for host-side binaries and for a possible upstream PS2 profile (`Docs/spikes/S0-rea-capability.md`). Installed with `npm install --global rea-agents`, registered with `rea setup --client claude_code`. |
| **PCSX2** 2.7.524 | Ground truth for behavior, debugger, GS dump capture, texture dumping, ISO sanity checks | GPL-3.0 | The oracle of last resort (architecture.md §15). `tools/pcsx2/headless_run.py` runs it headless under a private Xvfb with an isolated profile, scripted input, OSD OCR (tesseract), raw `.gs` dumps and uncompressed save states; `tools/gsdump/gsdump_stats.py` parses dumps into register, primitive and transfer histograms (S7). |
| **ImHex** | Hex editor with a pattern language for documenting the `HD6`, `MVI` and `MWo3` formats | GPL-2.0 | Patterns we write are committed under `tools/extract/`; the first is `hd6.hexpat` (spike S4; not yet verified in ImHex, which is not installed here). |
| **QuickBMS** | Quick archive probing once an index format is understood | Per its own terms | Dev use only. Our own extractor lives in `tools/extract`. |
| **GNU binutils** (`readelf`, `objdump`) | ELF inspection | GPL | Already used to inspect the boot ELF. |
| **ps2dev toolchain and SDK** | Build **our own** small PS2 test programs to use as synthetic fixtures and recompiler tests | Per its own terms; verify | Lets CI test the recompiler pipeline without any game data. |
| **Community PS2 hardware test programs** (for example `ps2autotests`) | Behavior checks for FPU, DMA and GS semantics | Verify before use | Candidate fixtures. |
| **RenderDoc** | Vulkan frame capture and debugging for the renderer | MIT | |
| **Vulkan validation layers**, `vulkaninfo` | Renderer correctness and capability reports | Apache-2.0 | |
| **objdiff**, **splat** | Only if we later pursue matching decompilation | MIT | One sibling decompilation project uses them. |
| **Linux perf**, Hotspot, heaptrack; **Superluminal** or Visual Studio profiler on Windows | CPU and memory profiling | Various | |

---

## 8. Testing and quality

| Area | Choice | Notes |
|---|---|---|
| Unit tests | **Catch2 v3**, run by **CTest** | Labels: `unit` (CI), `local` and `oracle` (need game data, local only). |
| Differential tests | Our reference interpreter as oracle | Architecture.md §15. |
| Sanitizers | AddressSanitizer, UndefinedBehaviorSanitizer on the runtime and core. ThreadSanitizer on render, audio and I/O threads. | Generated code exempt at first. |
| Fuzzing | **libFuzzer** targets for every parser that sees disc-image data: ISO 9660 reader, `MWo3` container, VIF and GIF decoders, TOML and settings loader | A disc image is untrusted input as far as a security review is concerned. |
| Static analysis | `clang-tidy`, CodeQL on GitHub | CodeQL is free for public repositories. |
| Coverage | `llvm-cov` or `gcov` | Tracked, not gated. |
| Style | `clang-format` with the committed `.clang-format`, `.editorconfig` | CI check. |
| License hygiene | SPDX identifiers in every source file, **REUSE** lint in CI | Supports the dependency policy (§10). |
| Link checks | Markdown link check in CI | Keeps the documentation honest. |

---

## 9. CI, packaging, release

### 9.1 Continuous integration (GitHub Actions)

| Job | Runs on | What |
|---|---|---|
| Build and unit tests | Ubuntu 24.04 (GCC, Clang), Windows (MSVC) | Configure, build, `ctest -L unit` |
| Style and lint | Ubuntu | clang-format check, clang-tidy |
| No-game-data check | Ubuntu | Heuristic scan (architecture.md §14) |
| License check | Ubuntu | REUSE lint, dependency license report |
| Sanitizers | Ubuntu | ASan and UBSan builds of the unit tests |
| Docs | Ubuntu | Markdown lint and link check |
| macOS | macOS runner, informational | Only once a Vulkan path exists |

**CI never has game data.** Local suites run on contributors' machines and before releases.

### 9.2 Packaging and release

| Platform | Artifact | Notes |
|---|---|---|
| Linux | **AppImage** built in an Ubuntu LTS container for broad compatibility, Flatpak later | A Flatpak sandbox must allow reading the user's disc image and invoking a compiler or the bundled toolchain, so it needs design work. |
| Windows | Portable zip, then an installer (Inno Setup or NSIS) | Optional code signing. |
| All | SHA-256 checksums, SBOM, changelog | Releases are produced from tags by CI in a reproducible container. A release job runs the no-game-data check over the produced archives. |

Versioning: **SemVer** for the product, with the **module ABI version** tracked separately (architecture.md §13.3).
Changelog follows Keep a Changelog. Issue and pull-request templates include the legal checklist.

---

## 10. Licensing and dependency policy

**Project license: GPL-3.0-or-later**, with `SPDX-License-Identifier` headers, REUSE-compliant.
Decision D2 in the brief, accepted 2026-10-19 as ADR-0009. The full text is in `LICENSE` at the repository root.

| Category | Policy |
|---|---|
| **Allowed** | MIT, BSD-2/3-Clause, zlib, BSL-1.0, Apache-2.0 (compatible with GPL-3.0), ISC, public domain, SIL OFL for fonts |
| **Allowed with conditions** | **LGPL-2.1+ and LGPL-3.0**: dynamic linking preferred, so users can substitute the library (paraLLEl-GS, FFmpeg). **GPL-2.0-or-later and GPL-3.0**: allowed because the project is GPL-3.0-or-later. |
| **Not allowed** | GPL-2.0-only (incompatible with GPL-3.0), non-commercial or no-derivatives licenses, proprietary SDKs with redistribution restrictions, code with no clear license |
| **FFmpeg** | If used, configured without `--enable-nonfree`. LGPL build preferred even though the GPL is permitted, to keep options open. |
| **Fonts and icons** | Original or SIL OFL. No Square Enix logos, artwork or fonts. |
| **Notices** | `THIRD_PARTY_NOTICES.md` lists every dependency, version and license. Updated in the same commit that adds or bumps a dependency. |
| **Contributions** | DCO sign-off, no contributor license agreement. |

A permissive license for our own code is only possible with a clean-room recompiler and runtime, which is not the
current plan. See architecture.md §18 (ADR-0009).

---

## 11. Alternatives considered

### 11.1 Overall approach

| Option | Verdict | Reason |
|---|---|---|
| **Static recompilation + native runtime** | **Chosen** | Native executable with feasible effort. Proven by sibling projects, though none is finished for a game of this scale. |
| Embed an emulator (PCSX2 core) | Rejected | Not a native port. Brings the full emulator and BIOS requirement. |
| Matching decompilation | Deferred | Stripped ELF with six overlays and no symbols is a very large effort. Overrides keep it possible later. |

### 11.2 Components

| Decision | Chosen | Alternatives | Why chosen |
|---|---|---|---|
| Recompiler | PS2Recomp | Writing our own (a sibling project uses a Rust translator); forking PS2Recomp | Active ecosystem, GPL-compatible, known workflow. A fork would diverge quickly. |
| Runtime language | C++20 | Rust | Generated code and PS2Recomp are C++. Rust would add an FFI boundary on the hottest path. |
| Launcher UI | Dear ImGui | **Qt 6** (LGPL, native look, large packaging cost); **RmlUi** (HTML/CSS-like, used by a sibling port, more setup); **Tauri or Electron** (second language and a heavy runtime); GTK, wxWidgets | One toolkit shared with the in-game overlay and no extra runtime. The front-end abstraction (ADR-0010) keeps Qt or a web UI possible. |
| Graphics API | Vulkan | **OpenGL 4.6** (used by two sibling ports, simpler, but parallel-gs needs Vulkan and macOS deprecates it); D3D12 or Metal (single-platform) | Required by the first backend. An OpenGL path for the native renderer can be added later. |
| Window and input | SDL3 | SDL2 (also installed here, but SDL3 supersedes it); GLFW (no audio, weaker gamepad support) | Fewer dependencies. |
| Audio | SDL3 audio streams | miniaudio (MIT, strong low-latency option); OpenAL Soft (LGPL); PortAudio | Already a dependency. Hidden behind `IAudioSink`, so swapping is local. |
| Build | CMake + Ninja | **Meson** (installed on the dev machine, pleasant, but PS2Recomp and most dependencies are CMake); Bazel (heavy) | Ecosystem fit. |
| Dependency management | Git submodules | vcpkg, Conan, CMake FetchContent | Reproducible, offline, one mechanism. FetchContent downloads at configure time, which is hostile to offline and audited builds. |
| Config format | TOML | JSON (no comments), YAML (ambiguity), INI (no types) | Typed, commented, already in the dependency set. |
| Guest threads | Fibers (minicoro) | OS threads with baton passing; boost.context | Cheap switches, deterministic scheduling. Revisit if deep guest stacks cause trouble. |
| First GS backend | paraLLEl-GS | Writing the native renderer first; porting a software renderer | Accurate and supersampling now. The native renderer is a long project that should not block M3. |

---

## 12. Development environment (observed 2026-10-19)

The primary development machine, as found by inspection. Treat it as the reference for "works on my machine", not as the minimum.

| Item | Found |
|---|---|
| OS | Linux 7.2.8 (CachyOS, Arch-based), fish shell |
| Compilers | GCC 16.2.1, Clang 23.1.1 |
| Build tools | CMake 4.4.3, Ninja 1.13.2, Meson 1.12.1 (not used by this project) |
| Scripting | Python 3.14.7, OpenJDK 27 |
| VCS and archive | Git 2.56.0, 7-Zip 26.03 |
| Graphics and platform | SDL3 3.4.16 and SDL2 2.32.72 development packages, Vulkan 1.4.357 (headers and loader), `glslc` 2026.3 |
| GPU | NVIDIA GeForce RTX 4060 |
| **Found in the sibling `MGS2_Undub` project** | Ghidra 12.1.4 with the Emotion Engine extension (`tools/ghidra_12.1.4_PUBLIC`), an extracted PCSX2 2.7.524 (`tools/pcsx2/squashfs-root`), a headless PCSX2 harness and headless Ghidra scripts; the PS2 BIOS dump and the PCSX2 AppImage live under the home directory. All referenced through `tools/local.env`, never copied into this repository. |
| **Found** | `clang-format`, `clang-tidy`, `vulkaninfo`, Xvfb, ffmpeg, podman and docker, Node.js 24.21 (for REA); paraLLEl-GS and its tools built under `work/parallel-gs/` (S5a); a Ghidra project of the boot ELF under `work/ghidra/base` (S2); `tesseract` for OSD OCR; Xvfb works inside the Bash sandbox (S7) |
| **Not found** | `spirv-cross`, `xorriso` and `isoinfo` (the last two are not needed: our own ISO reader replaces them); `pre-commit`, `reuse`, `ccache`, `gh` (being installed with pacman) |
| **Not checked** | RenderDoc, ImHex, a PS2 cross toolchain |

Notes:

- **JDK:** Ghidra 12.1.4 declares JDK 21 as the minimum and no maximum; REA's doctor accepts the installed JDK 27 and the sibling project runs Ghidra with it. Spike S2 confirms headless analysis under JDK 27; `DQ8_JAVA_HOME` in `tools/local.env` selects another JDK if needed.
- **Compilers are newer than any LTS distribution.** CI must cover Ubuntu 24.04's toolchain too (§4.1).
- **Disc image:** the working copy lives in `Game/` (git-ignored) as both an ISO and a 7z archive of it. Only the ISO is used.

**Version pinning.** Submodule commits are the source of truth. Each bump is its own commit with a note in `THIRD_PARTY_NOTICES.md`.
Tool versions used by CI (compilers, CMake, Ghidra for developer docs) are recorded in one place, to be created with the first CI workflow.

---

## 13. Spikes that validate the stack

The brief's M0 spikes (project-brief.md §11) are also the checks on this document. Each can overturn a choice here.

| Spike | Validates | A bad result means |
|---|---|---|
| **S1** PS2Recomp builds and the game-module compile cost | CMake and compiler choices, §6 build settings, bundled-toolchain need | Rework chunking, adopt option B (§6.3) sooner, or patch PS2Recomp harder |
| **S2** Ghidra loads `SLUS_212.07` and exports functions | Ghidra plus EE plugin, JDK choice | Pick another analysis path (the PS2Recomp analyzer, or a different plugin or version) |
| **S3** Overlay loader and cache-flush behavior | Overlay design (architecture.md §4.3) | Choose the content-fingerprint fallback as primary |
| **S4** `DATA.DAT` index | The `tools/extract` scope, I/O design | Fall back to extraction at setup |
| **S5** paraLLEl-GS standalone replay and embedding | Backend choice D5, GPU requirements | Start the native renderer earlier, or require a higher GPU baseline |
| **S6** `MVI` movie format | Movie decoder dependency | Add or switch the decoder library |
| **S7** Real frame rate, resolution and GS usage in PCSX2 | Performance budgets, interpolation feasibility | Re-plan the presentation pipeline and enhancement priorities |
