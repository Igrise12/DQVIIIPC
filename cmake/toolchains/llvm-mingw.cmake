# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# x86-64 Windows with llvm-mingw (clang, lld, mingw-w64, libc++; ADR-0018). One file for both uses:
#
#   - cross-building the host from Linux (CI's windows-cross job, contributors):
#       cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw.cmake \
#             -DLLVM_MINGW_ROOT=/path/to/llvm-mingw-<version>-ucrt-ubuntu-22.04-x86_64
#   - building the game modules on the user's Windows machine with the downloaded bundle
#     (src/core/toolchain/download.h): the setup passes the same two variables to cmake/module.
#
# LLVM_MINGW_ROOT is the unpacked release (the directory holding bin/ and x86_64-w64-mingw32/); the environment
# variable of the same name is used when the cache variable is not set. The pinned release is in
# llvm-mingw-pin.cmake; packaging/windows/fetch_tools.cmake downloads it.
#
# On Linux, Wine (when found) is the CMAKE_CROSSCOMPILING_EMULATOR, so ctest and Catch2's test discovery run the
# Windows executables. The C++ runtime is libc++ as DLLs (libc++.dll, libunwind.dll): host and modules share one
# runtime and one exception model, which the module boundary needs. The emulator command puts the toolchain's DLL
# directory on WINEPATH; tests that start .exe files themselves need WINEPATH in their environment as well
# (packaging/windows/README.md).

if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
    set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if(NOT LLVM_MINGW_ROOT)
    message(FATAL_ERROR "llvm-mingw: set LLVM_MINGW_ROOT (cache or environment) to the unpacked llvm-mingw release; "
                        "packaging/windows/fetch_tools.cmake downloads the pinned one")
endif()
file(TO_CMAKE_PATH "${LLVM_MINGW_ROOT}" LLVM_MINGW_ROOT)
set(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" CACHE PATH "The unpacked llvm-mingw release")
# try_compile() projects re-read this file without the cache: hand the root on to them.
set(ENV{LLVM_MINGW_ROOT} "${LLVM_MINGW_ROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT)

set(LLVM_MINGW_TRIPLE "x86_64-w64-mingw32")
if(CMAKE_HOST_WIN32)
    set(_dq8_exe ".exe")
else()
    set(_dq8_exe "")
    # Cross compiling. On Windows itself the native platform is already the target.
    set(CMAKE_SYSTEM_NAME Windows)
    set(CMAKE_SYSTEM_VERSION 10.0)
endif()
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_dq8_bin "${LLVM_MINGW_ROOT}/bin")
if(NOT EXISTS "${_dq8_bin}/${LLVM_MINGW_TRIPLE}-clang++${_dq8_exe}")
    message(FATAL_ERROR "llvm-mingw: '${LLVM_MINGW_ROOT}' has no bin/${LLVM_MINGW_TRIPLE}-clang++${_dq8_exe}")
endif()
set(CMAKE_C_COMPILER "${_dq8_bin}/${LLVM_MINGW_TRIPLE}-clang${_dq8_exe}")
set(CMAKE_CXX_COMPILER "${_dq8_bin}/${LLVM_MINGW_TRIPLE}-clang++${_dq8_exe}")
set(CMAKE_RC_COMPILER "${_dq8_bin}/${LLVM_MINGW_TRIPLE}-windres${_dq8_exe}")
set(CMAKE_AR "${_dq8_bin}/llvm-ar${_dq8_exe}" CACHE FILEPATH "llvm-mingw archiver")
set(CMAKE_RANLIB "${_dq8_bin}/llvm-ranlib${_dq8_exe}" CACHE FILEPATH "llvm-mingw ranlib")

# Every executable links windows-app.manifest (the UTF-8 code page, long paths), compiled here once per build
# tree. DQ8_WINDOWS_MANIFEST=OFF leaves it out.
option(DQ8_WINDOWS_MANIFEST "Link the UTF-8 application manifest into every executable" ON)
if(DQ8_WINDOWS_MANIFEST AND NOT CMAKE_IN_TRY_COMPILE)
    set(_dq8_manifest_dir "${CMAKE_BINARY_DIR}/dq8-windows-manifest")
    set(_dq8_manifest_object "${_dq8_manifest_dir}/windows-app.res.o")
    file(MAKE_DIRECTORY "${_dq8_manifest_dir}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/windows-app.manifest" "${_dq8_manifest_dir}/windows-app.manifest"
                   COPYONLY)
    file(WRITE "${_dq8_manifest_dir}/windows-app.rc" "1 24 \"windows-app.manifest\"\n") # RT_MANIFEST, ID 1
    execute_process(COMMAND "${CMAKE_RC_COMPILER}" -O coff -i windows-app.rc -o windows-app.res.o
                    WORKING_DIRECTORY "${_dq8_manifest_dir}" RESULT_VARIABLE _dq8_rc OUTPUT_QUIET ERROR_QUIET)
    if(_dq8_rc EQUAL 0)
        string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " \"${_dq8_manifest_object}\"")
    else()
        message(WARNING "llvm-mingw: windres could not compile the application manifest; executables get none")
    endif()
endif()

# Libraries and headers come from the toolchain's sysroot (plus DQ8_WINDOWS_PREFIX_PATH, e.g. a cross-built
# FFmpeg); programs (pkg-config, python, git) from the build machine.
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/${LLVM_MINGW_TRIPLE}" ${DQ8_WINDOWS_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config must not report the build machine's libraries (cmake/Movie.cmake probes libavcodec with it).
if(NOT CMAKE_HOST_WIN32)
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
    set(_dq8_pc_dirs "")
    foreach(_dq8_prefix IN LISTS DQ8_WINDOWS_PREFIX_PATH)
        list(APPEND _dq8_pc_dirs "${_dq8_prefix}/lib/pkgconfig")
    endforeach()
    if(_dq8_pc_dirs)
        list(JOIN _dq8_pc_dirs ":" _dq8_pc_dirs)
        set(ENV{PKG_CONFIG_LIBDIR} "${_dq8_pc_dirs}")
    else()
        set(ENV{PKG_CONFIG_LIBDIR} "${LLVM_MINGW_ROOT}/${LLVM_MINGW_TRIPLE}/lib/pkgconfig")
    endif()
    set(ENV{PKG_CONFIG_PATH} "")
endif()

# Run the Windows executables under Wine when cross compiling (tests, Catch2 discovery). DQ8_WINE overrides.
if(NOT CMAKE_HOST_WIN32 AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    if(NOT DQ8_WINE)
        find_program(DQ8_WINE NAMES wine wine64 NO_CMAKE_FIND_ROOT_PATH)
    endif()
    if(DQ8_WINE)
        find_program(_dq8_env_program env NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        find_program(_dq8_sh_program sh NO_CMAKE_FIND_ROOT_PATH REQUIRED)
        # Z: is Wine's view of the Unix root; libc++.dll and libunwind.dll live in the triple's bin/. A new Wine
        # prefix would otherwise stop at a dialog offering to install Mono and Gecko, and Mesa's EGL warnings from
        # Wine's display driver would come before a GUI program's output. wine-run.sh turns path arguments into Z:
        # paths.
        set(CMAKE_CROSSCOMPILING_EMULATOR
            "${_dq8_env_program}" "WINEPATH=Z:${LLVM_MINGW_ROOT}/${LLVM_MINGW_TRIPLE}/bin" "WINEDEBUG=-all"
            "WINEDLLOVERRIDES=mscoree,mshtml=" "EGL_LOG_LEVEL=fatal" "${_dq8_sh_program}"
            "${CMAKE_CURRENT_LIST_DIR}/wine-run.sh" "${DQ8_WINE}")
    endif()
endif()
