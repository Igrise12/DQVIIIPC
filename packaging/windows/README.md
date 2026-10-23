# packaging/windows/

The portable Windows zip, cross-built from Linux with the pinned llvm-mingw (ADR-0018, Docs/milestones/M5-launcher.md
session E). Contributor tools only; nothing here runs on a player's setup path except what ends up in the zip.

| File | Purpose |
|---|---|
| `fetch_tools.cmake` | Downloads and unpacks the pinned tools of `cmake/toolchains/llvm-mingw-pin.cmake`: the Linux-hosted llvm-mingw (the cross compiler), and the Windows cmake and ninja that ship in the zip; every file checked against its SHA-256 |
| `make_zip.cmake` | Assembles the zip from a cross build and those tools, checks that every imported DLL is a Windows DLL or shipped, writes the zip and its `.sha256` |
| `README-windows.txt` | The `README.txt` inside the zip |
| `cross-build-modules.sh` | Builds the game modules of an existing recompile work directory with the cross compiler, for smoke runs under Wine (the output is derived from your disc: keep it in `work/`) |

The CI job is `.github/workflows/windows-cross.yml`: it cross-builds, runs the unit tests under Wine, builds the
zip and scans it with `scripts/check_no_game_data.py --release --archive` (host executables and libraries pass;
anything game-shaped is refused by content; `.no-game-data-allow` covers only the CMake release's own files).

## Cross-building and testing under Wine

```bash
cmake -DDEST=work/tools -P packaging/windows/fetch_tools.cmake
export LLVM_MINGW_ROOT="$PWD/work/tools/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64"
cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/llvm-mingw.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DDQ8_FFMPEG=off -DDQ8_BUILD_RECOMP=ON \
      -DDQ8_TEST_WINDOWS_CMAKE="$PWD/work/tools/cmake-4.4.4-windows-x86_64/bin/cmake.exe"
cmake --build build/windows --parallel
WINEPATH="Z:$LLVM_MINGW_ROOT/x86_64-w64-mingw32/bin" SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy \
    ctest --test-dir build/windows -L unit -j8
cmake -DBUILD=build/windows -DTOOLS=work/tools -DOUT=build/windows/package -P packaging/windows/make_zip.cmake
```

- **Wine.** The toolchain file makes Wine the `CMAKE_CROSSCOMPILING_EMULATOR` (through `wine-run.sh`, which hands
  absolute paths to the program as `Z:` paths: Catch2 on Windows reads a leading `/` as an option). Programs that a test's
  CMake script starts (`run_and_match.cmake` and the like) go through generated wrappers in `build/.../tests/run/`
  that call the same emulator (`dq8_test_program` in `tests/unit/CMakeLists.txt`), so no binfmt_misc registration
  is needed; `WINEPATH` names the toolchain's DLL directory. A fresh `WINEPREFIX`
  asks to install Mono unless `WINEDLLOVERRIDES=mscoree,mshtml=` is set. Wine maps file names through the Unix
  locale, so the non-ASCII path tests need a UTF-8 one (`LANG=C.UTF-8`).
- **What Wine cannot show.** Wine's `vulkan-1.dll` passes the host's GPU through and ignores the Vulkan loader's
  variables, so the "no Vulkan driver" tests skip there; real Windows runs them. Exception unwinding, fibers,
  job objects, WinHTTP and the UTF-8 code page all run under Wine, but the acceptance run is on real Windows (I3).

## The zip

```text
DQ8-Native-<version>-windows-x86_64/
  dq8-launcher.exe  dq8-game.exe  dq8ctl.exe  ps2_recomp.exe     the programs (the launcher is a GUI-subsystem program)
  dq8-gs-parallel.dll (+ .COPYING.LGPLv3, .NOTICE.md)             the LGPL GS backend, replaceable
  dq8-movie.dll, avcodec/avutil DLLs                               when built with the LGPL FFmpeg (session D)
  libc++.dll  libunwind.dll                                        llvm-mingw's C++ runtime, shared with the modules
  data/  config/                                                   region profiles, maps, settings schema and strings
  cmake/module/  cmake/toolchains/  include/dq8/abi/               what the setup needs to build the game modules
  tools/cmake/  tools/ninja/                                       the bundled build tools
  README.txt  LICENSE.txt  THIRD_PARTY_NOTICES.md  LICENSES/
```

The layout is flat because the programs look for `data/`, `cmake/module` and `include/` beside themselves or in an
ancestor directory (`dq8ctl`'s and `dq8-game`'s searches), and Windows finds a program's DLLs in its own directory.
The compiler is not in the zip: the setup downloads the pinned Windows-hosted llvm-mingw with the user's consent
into `%APPDATA%\DQ8-Native\toolchain\<version>\` (`src/core/toolchain/download.h`), and builds the modules with it,
`tools/cmake`, `tools/ninja` and `cmake/toolchains/llvm-mingw.cmake`, so host and modules share one compiler,
`libc++.dll` and exception model.

Executables built with the toolchain file carry `cmake/toolchains/windows-app.manifest`: the UTF-8 code page (so
`argv`, the environment and narrow file names are UTF-8 as on Linux; Windows 10 1903 or later) and long paths.

## Bumping the pins

Change `cmake/toolchains/llvm-mingw-pin.cmake` and `pinned_llvm_mingw()` in `src/core/toolchain/download.cpp`
together (a unit test compares them), with the SHA-256 values GitHub publishes for the release assets and the
Windows archive's size, unpacked size and entry count.
