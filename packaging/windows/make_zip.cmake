# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Assembles the portable Windows zip (ADR-0018) from a cross build (cmake/toolchains/llvm-mingw.cmake) and the
# pinned tools (fetch_tools.cmake):
#
#   cmake -DBUILD=<build dir> -DTOOLS=<fetch_tools DEST> -DOUT=<dir> [-DVERSION=x.y.z] -P make_zip.cmake
#
# Layout (README.md has the reasons): the programs and their DLLs at the top, then data/, config/, cmake/module,
# cmake/toolchains, include/dq8/abi, tools/cmake, tools/ninja and the licences. Every DLL a program or library
# imports must be a Windows system DLL or shipped in the zip; the script fails otherwise. Output:
# <OUT>/DQ8-Native-<version>-windows-x86_64.zip and its .sha256. Nothing from the user's disc is read.
cmake_minimum_required(VERSION 3.25)
foreach(_var BUILD TOOLS OUT)
    if(NOT ${_var})
        message(FATAL_ERROR "usage: cmake -DBUILD=<build> -DTOOLS=<tools> -DOUT=<dir> [-DVERSION=x.y.z] -P make_zip.cmake")
    endif()
    get_filename_component(${_var} "${${_var}}" ABSOLUTE)
endforeach()
get_filename_component(SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SOURCE}/cmake/toolchains/llvm-mingw-pin.cmake")
if(NOT VERSION)
    file(STRINGS "${SOURCE}/CMakeLists.txt" _project REGEX "^project\\(dq8-native VERSION ")
    string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" VERSION "${_project}")
endif()

set(_llvm_mingw "${TOOLS}/llvm-mingw-${DQ8_LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64")
set(_cmake "${TOOLS}/cmake-${DQ8_WINDOWS_CMAKE_VERSION}-windows-x86_64")
set(_ninja "${TOOLS}/ninja-${DQ8_WINDOWS_NINJA_VERSION}")
set(_objdump "${_llvm_mingw}/bin/llvm-objdump")
foreach(_need "${_objdump}" "${_cmake}/bin/cmake.exe" "${_ninja}/ninja.exe" "${_ninja}/COPYING")
    if(NOT EXISTS "${_need}")
        message(FATAL_ERROR "missing ${_need}: run packaging/windows/fetch_tools.cmake with DEST=${TOOLS}")
    endif()
endforeach()

set(_name "DQ8-Native-${VERSION}-windows-x86_64")
set(_stage "${OUT}/${_name}")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}")

# --- Programs and our libraries -------------------------------------------------------------------------------
# name -> path in the build tree; ps2_recomp and dq8-movie are optional (DQ8_BUILD_RECOMP, DQ8_FFMPEG).
set(_required src/host/dq8-game.exe src/launcher/dq8-launcher.exe src/cli/dq8ctl.exe src/host/dq8-gs-parallel.dll)
set(_optional ps2recomp/build/ps2xRecomp/ps2_recomp.exe src/host/dq8-movie.dll)
foreach(_file IN LISTS _required _optional)
    if(EXISTS "${BUILD}/${_file}")
        file(COPY "${BUILD}/${_file}" DESTINATION "${_stage}")
    elseif(_file IN_LIST _required)
        message(FATAL_ERROR "the build has no ${_file}")
    else()
        message(STATUS "not in the build, left out: ${_file}")
    endif()
endforeach()
# The LGPL text and notice that travel with dq8-gs-parallel (and dq8-movie's, when built).
file(GLOB _library_notices "${BUILD}/src/host/dq8-gs-parallel.*" "${BUILD}/src/host/dq8-movie.*")
list(FILTER _library_notices EXCLUDE REGEX "\\.(dll|a)$")
file(COPY ${_library_notices} DESTINATION "${_stage}")
# A minimal LGPL FFmpeg for dq8-movie (M5 session D), when given: -DFFMPEG_DLLS=<dir with avcodec-*.dll ...>.
if(FFMPEG_DLLS)
    file(GLOB _ffmpeg "${FFMPEG_DLLS}/*.dll")
    file(COPY ${_ffmpeg} DESTINATION "${_stage}")
endif()

# --- Data, settings, the module template and its toolchain file, the ABI headers -------------------------------
file(COPY "${SOURCE}/data" "${SOURCE}/config" DESTINATION "${_stage}")
file(COPY "${SOURCE}/cmake/module" DESTINATION "${_stage}/cmake")
file(COPY "${SOURCE}/cmake/toolchains/llvm-mingw.cmake" "${SOURCE}/cmake/toolchains/llvm-mingw-pin.cmake"
          "${SOURCE}/cmake/toolchains/windows-app.manifest" DESTINATION "${_stage}/cmake/toolchains")
file(COPY "${SOURCE}/include/dq8/abi" DESTINATION "${_stage}/include/dq8")

# --- CMake (cmake.exe and its modules; no GUI, CTest or CPack) and Ninja ----------------------------------------
file(COPY "${_cmake}/bin/cmake.exe" DESTINATION "${_stage}/tools/cmake/bin")
file(COPY "${_cmake}/share" DESTINATION "${_stage}/tools/cmake")
file(COPY "${_cmake}/doc/cmake" DESTINATION "${_stage}/tools/cmake/doc"
     PATTERN "html" EXCLUDE PATTERN "*.qch" EXCLUDE PATTERN "*.html" EXCLUDE)
file(COPY "${_ninja}/ninja.exe" "${_ninja}/COPYING" DESTINATION "${_stage}/tools/ninja")

# --- Licences ---------------------------------------------------------------------------------------------------
file(COPY_FILE "${SOURCE}/LICENSE" "${_stage}/LICENSE.txt")
file(COPY "${SOURCE}/THIRD_PARTY_NOTICES.md" "${SOURCE}/LICENSES" DESTINATION "${_stage}")
file(COPY_FILE "${_llvm_mingw}/LICENSE.TXT" "${_stage}/LICENSES/llvm-mingw-runtime.txt")
file(COPY_FILE "${CMAKE_CURRENT_LIST_DIR}/README-windows.txt" "${_stage}/README.txt")

# --- Every imported DLL is a system DLL or in the zip (the llvm-mingw runtime is copied from the toolchain) -------
set(_system_dll "^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|imm32|version|winmm|setupapi|hid|winhttp|ws2_32|dwmapi|uxtheme|cfgmgr32|shlwapi|comdlg32|bcrypt|ntdll|dbghelp|dxgi|d3d11|d3d12|dinput8|xinput1_4|winspool|uuid|crypt32|secur32|psapi|powrprof|iphlpapi|userenv|rpcrt4|api-ms-win-.*|ucrtbase)\\.dll$")
set(_runtime_dir "${_llvm_mingw}/x86_64-w64-mingw32/bin")
set(_checked "")
set(_pending "")
file(GLOB _binaries "${_stage}/*.exe" "${_stage}/*.dll" "${_stage}/tools/*/*.exe" "${_stage}/tools/cmake/bin/*.exe")
list(APPEND _pending ${_binaries})
while(_pending)
    list(POP_FRONT _pending _binary)
    execute_process(COMMAND "${_objdump}" -p "${_binary}" OUTPUT_VARIABLE _dump RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "llvm-objdump failed on ${_binary}")
    endif()
    string(REGEX MATCHALL "DLL Name: [^\n]+" _imports "${_dump}")
    foreach(_import IN LISTS _imports)
        string(REPLACE "DLL Name: " "" _dll "${_import}")
        string(STRIP "${_dll}" _dll)
        string(TOLOWER "${_dll}" _lower)
        if(_lower IN_LIST _checked OR _lower MATCHES "${_system_dll}")
            continue()
        endif()
        list(APPEND _checked "${_lower}")
        file(GLOB _present "${_stage}/${_dll}")
        if(_present)
            continue()
        endif()
        if(EXISTS "${_runtime_dir}/${_dll}")
            file(COPY "${_runtime_dir}/${_dll}" DESTINATION "${_stage}")
            list(APPEND _pending "${_stage}/${_dll}")
            message(STATUS "runtime DLL: ${_dll}")
        else()
            message(FATAL_ERROR "${_binary} imports ${_dll}, which is neither a Windows DLL nor in the zip")
        endif()
    endforeach()
endwhile()

# --- The zip and its digest -------------------------------------------------------------------------------------
set(_zip "${OUT}/${_name}.zip")
file(REMOVE "${_zip}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${_zip}" --format=zip "${_name}"
                WORKING_DIRECTORY "${OUT}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "zipping ${_stage} failed")
endif()
file(SHA256 "${_zip}" _digest)
file(WRITE "${_zip}.sha256" "${_digest}  ${_name}.zip\n")
file(SIZE "${_zip}" _size)
message(STATUS "${_zip}: ${_size} bytes, SHA-256 ${_digest}")
