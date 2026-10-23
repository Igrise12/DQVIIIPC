# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# libdq8core: platform-neutral foundation (configuration, verification, setup orchestration).
# Knows nothing about the PS2 beyond file formats and nothing about any UI. See src/core/README.md and
# architecture.md §13.
add_library(dq8_core STATIC
    version.cpp
    error.cpp
    hash/hash.cpp
    iso/iso9660.cpp
    elf/elf_headers.cpp
    elf/elf_writer.cpp
    profile/region_profile.cpp
    overlay/mwo3.cpp
    overlay/overlay_elf.cpp
    platform/files.cpp
    platform/paths.cpp
    platform/process.cpp
    platform/system_info.cpp
    recomp/function_map.cpp
    recomp/recomp_config.cpp
    recomp/recomp_driver.cpp
    recomp/module_build.cpp
    recomp/units.cpp
    recomp/pipeline.cpp
    intake/verify.cpp
    intake/extract.cpp
    error_text.cpp
    settings/settings.cpp
    setup/setup.cpp
    setup/diagnostics.cpp
    toolchain/toolchain.cpp
    registry/registry.cpp
    input/input_profile.cpp
)
add_library(dq8::core ALIAS dq8_core)
target_include_directories(dq8_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_include_directories(dq8_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(dq8_core PUBLIC cxx_std_20)
target_compile_definitions(dq8_core PRIVATE DQ8_VERSION="${PROJECT_VERSION}")
target_link_libraries(dq8_core PRIVATE toml11::toml11)
# The emitter's call list, for recomp_config.cpp. Session A adds include/dq8/abi/ps2recomp/ps2_call_list.h;
# until that is merged, the verbatim stand-in under tests/unit/support/ is used. Remove after the merge.
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/include/dq8/abi/ps2recomp/ps2_call_list.h")
    target_include_directories(dq8_core PRIVATE "${PROJECT_SOURCE_DIR}/tests/unit/support")
endif()
dq8_set_warnings(dq8_core)
dq8_enable_sanitizers(dq8_core)
# M5 session E: the bundled Windows toolchain's download (src/core/toolchain/download.h; ADR-0018), over WinHTTP
# on Windows.
target_sources(dq8_core PRIVATE toolchain/download.cpp)
if(WIN32)
    target_link_libraries(dq8_core PRIVATE winhttp)
endif()
