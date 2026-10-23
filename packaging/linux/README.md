# packaging/linux

The Linux release: one AppImage (`DQ8-Native-<version>-x86_64.AppImage`) built on Ubuntu 24.04.

| File | Role |
|---|---|
| `build-appimage.sh` | Installs a built tree into an AppDir, adds CMake and Ninja, the desktop entry, icon and `AppRun`, scans the AppDir, packs it with appimagetool and scans the result. Downloads are pinned by version and SHA-256 |
| `AppRun` | Starts `dq8-launcher`; `<AppImage> dq8ctl ...` (or `dq8-game`, `ps2_recomp`, `cmake`, `ninja`) runs a bundled tool. Puts the bundled CMake and Ninja first on `PATH` |
| `dq8-native.desktop`, `dq8-native.svg` | The desktop entry and the icon (drawn for this project) |

```bash
bash scripts/init_submodules.sh
git submodule update --init --checkout --depth 1 third_party/ffmpeg
cmake --preset release -B build/appimage -DDQ8_BUILD_RECOMP=ON -DDQ8_FFMPEG=bundled -DDQ8_BUILD_TESTS=OFF
cmake --build build/appimage --parallel
packaging/linux/build-appimage.sh --build-dir build/appimage --out build/appimage-out
```

The build needs the CI's Linux packages (`.github/workflows/ci.yml`: SDL3's headers), a C and C++ compiler, CMake,
Ninja, make, curl, unzip and Python 3.11. The CI job is `.github/workflows/appimage.yml`.

## What is inside

```
AppRun, dq8-native.desktop, dq8-native.svg, .DirIcon
usr/bin/      dq8-launcher, dq8-game, dq8ctl, ps2_recomp, cmake, ninja
usr/lib/      libdq8-gs-parallel.so, libdq8-movie.so, libavcodec.so.*, libavutil.so.* (LGPL FFmpeg, mpeg2video only)
usr/share/dq8/        regions/, maps/, settings/, cmake/module/, include/dq8/abi/
usr/share/cmake-*/    CMake's modules
usr/share/doc/dq8-native/  LICENSE, THIRD_PARTY_NOTICES.md, licenses/, the libraries' notices and FFmpeg's build line
```

The user's system provides the C++ compiler that builds the game modules (the setup's toolchain check says what to
install), the Vulkan driver, and the display and audio libraries SDL loads at run time. The game modules, the
extracted disc files and the saves live in the user's data directory, never inside the AppImage. Release artifacts
hold no game data: `scripts/check_no_game_data.py --release` refuses MIPS code, disc file names and hashes, and
generated code.
