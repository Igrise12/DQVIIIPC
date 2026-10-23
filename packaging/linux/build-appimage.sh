#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Builds the DQ8-Native AppImage from a configured and built CMake tree (Docs/milestones/M5-launcher.md, session D).
#
#   cmake --preset release -B build/appimage -DDQ8_BUILD_RECOMP=ON -DDQ8_FFMPEG=bundled -DDQ8_BUILD_TESTS=OFF
#   cmake --build build/appimage --parallel
#   packaging/linux/build-appimage.sh --build-dir build/appimage --out build/appimage-out
#
# The AppImage holds the launcher, dq8-game, dq8ctl, ps2_recomp, the GS and movie libraries (with the bundled LGPL
# FFmpeg), share/dq8 (region profiles, published maps, settings, the module template and the ABI headers), and
# CMake and Ninja for the module build; the user's own C++ compiler builds the game modules. Nothing from a game
# disc is in it: the script scans the AppDir and the finished AppImage with scripts/check_no_game_data.py --release.
#
# Downloads (pinned, SHA-256 checked, cached in --cache): CMake and Ninja, appimagetool and the AppImage runtime.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/packaging/linux"

CMAKE_VERSION=4.4.4
CMAKE_URL="https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz"
CMAKE_SHA256=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb
NINJA_VERSION=1.13.2
NINJA_URL="https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-linux.zip"
NINJA_SHA256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
NINJA_LICENSE_URL="https://raw.githubusercontent.com/ninja-build/ninja/v$NINJA_VERSION/COPYING"
NINJA_LICENSE_SHA256=eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea
APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME_VERSION=20251108
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-x86_64"
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUNTIME_LICENSE_URL="https://raw.githubusercontent.com/AppImage/type2-runtime/$RUNTIME_VERSION/LICENSE"
RUNTIME_LICENSE_SHA256=aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1

usage() {
    echo "usage: $0 --build-dir DIR [--out DIR] [--cache DIR] [--version TEXT]" >&2
    exit 2
}

BUILD_DIR=""
OUT_DIR="$ROOT/build/appimage-out"
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/dq8-native-packaging"
VERSION=""
while [ $# -gt 0 ]; do
    case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --cache) CACHE_DIR="$2"; shift 2 ;;
    --version) VERSION="$2"; shift 2 ;;
    *) usage ;;
    esac
done
[ -n "$BUILD_DIR" ] || usage
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
for tool in dq8-game dq8ctl dq8-launcher; do
    if ! find "$BUILD_DIR/src" -name "$tool" -type f -perm -u+x | grep -q .; then
        echo "build-appimage: $BUILD_DIR has no $tool; build the tree first" >&2
        exit 1
    fi
done
if [ -z "$VERSION" ]; then
    VERSION="$("$(find "$BUILD_DIR/src/cli" -name dq8ctl -type f | head -1)" --version | awk '{print $NF}')"
fi

# fetch URL SHA256 FILE: download into the cache once, check the hash every time.
fetch() {
    local url="$1" sha="$2" file="$CACHE_DIR/$3"
    mkdir -p "$CACHE_DIR"
    if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    if ! echo "$sha  $file" | sha256sum -c --quiet -; then
        echo "build-appimage: $url does not have the pinned SHA-256 $sha" >&2
        exit 1
    fi
    echo "$file"
}

cmake_tgz="$(fetch "$CMAKE_URL" "$CMAKE_SHA256" "cmake-$CMAKE_VERSION-linux-x86_64.tar.gz")"
ninja_zip="$(fetch "$NINJA_URL" "$NINJA_SHA256" "ninja-$NINJA_VERSION-linux.zip")"
ninja_license="$(fetch "$NINJA_LICENSE_URL" "$NINJA_LICENSE_SHA256" "ninja-$NINJA_VERSION-COPYING")"
appimagetool="$(fetch "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256" "appimagetool-$APPIMAGETOOL_VERSION-x86_64.AppImage")"
runtime="$(fetch "$RUNTIME_URL" "$RUNTIME_SHA256" "runtime-$RUNTIME_VERSION-x86_64")"
runtime_license="$(fetch "$RUNTIME_LICENSE_URL" "$RUNTIME_LICENSE_SHA256" "type2-runtime-$RUNTIME_VERSION-LICENSE")"
chmod +x "$appimagetool"

APPDIR="$OUT_DIR/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR"
echo "== install into $APPDIR/usr"
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"
licenses="$APPDIR/usr/share/doc/dq8-native/licenses"
mkdir -p "$licenses"

echo "== CMake $CMAKE_VERSION and Ninja $NINJA_VERSION"
work="$OUT_DIR/unpack"
rm -rf "$work"
mkdir -p "$work"
tar -xzf "$cmake_tgz" -C "$work"
cmake_root="$work/cmake-$CMAKE_VERSION-linux-x86_64"
cp "$cmake_root/bin/cmake" "$APPDIR/usr/bin/cmake"
cmake_share="$APPDIR/usr/share/cmake-${CMAKE_VERSION%.*}"
cp -r "$cmake_root/share/cmake-${CMAKE_VERSION%.*}" "$cmake_share"
# CMake's few binary files are templates of the Visual Studio, WinRT and macOS packaging paths, which the
# module build (Ninja, Linux) never reads; leaving them out keeps the release scan's binary rule strict.
python3 -I - "$cmake_share" <<'PY'
import sys
from pathlib import Path
for f in sorted(Path(sys.argv[1]).rglob("*")):
    if f.is_file() and not f.is_symlink() and b"\0" in f.read_bytes()[:8192]:
        print(f"  left out of the bundled CMake: {f.relative_to(sys.argv[1])}")
        f.unlink()
PY
# CMake's licence and those of the libraries it links statically.
(cd "$cmake_root/doc/cmake" && find . -type f \( -name 'LICENSE*' -o -name 'Copyright*' -o -name 'COPYING*' \) \
    -exec install -D -m 644 {} "$licenses/cmake/{}" \;)
(cd "$work" && unzip -q "$ninja_zip")
install -m 755 "$work/ninja" "$APPDIR/usr/bin/ninja"
cp "$ninja_license" "$licenses/ninja-COPYING"
cp "$runtime_license" "$licenses/appimage-type2-runtime-LICENSE"
rm -rf "$work"

echo "== desktop entry, icon, AppRun"
install -m 755 "$HERE/AppRun" "$APPDIR/AppRun"
install -m 644 "$HERE/dq8-native.desktop" "$APPDIR/dq8-native.desktop"
install -m 644 "$HERE/dq8-native.svg" "$APPDIR/dq8-native.svg"
ln -sf dq8-native.svg "$APPDIR/.DirIcon"
install -D -m 644 "$HERE/dq8-native.desktop" "$APPDIR/usr/share/applications/dq8-native.desktop"
install -D -m 644 "$HERE/dq8-native.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/dq8-native.svg"

echo "== no-game-data scan of the AppDir"
python3 -I "$ROOT/scripts/check_no_game_data.py" --release "$APPDIR"

APPIMAGE="$OUT_DIR/DQ8-Native-$VERSION-x86_64.AppImage"
echo "== $APPIMAGE"
rm -f "$APPIMAGE"
# appimagetool is itself an AppImage: extract-and-run needs no FUSE (CI runners and containers).
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$appimagetool" --no-appstream --runtime-file "$runtime" "$APPDIR" "$APPIMAGE"

echo "== no-game-data scan of the AppImage's contents"
check="$OUT_DIR/scan"
rm -rf "$check"
mkdir -p "$check"
(cd "$check" && "$APPIMAGE" --appimage-extract > /dev/null)
python3 -I "$ROOT/scripts/check_no_game_data.py" --release "$check/squashfs-root"
rm -rf "$check"
echo "built $APPIMAGE ($(du -h "$APPIMAGE" | cut -f1))"
