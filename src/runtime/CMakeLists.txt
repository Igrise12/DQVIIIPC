# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8_runtime: the PS2 machine as the host implements it (architecture.md §4, §5). Knows nothing about Dragon
# Quest: region- and game-specific facts arrive from src/game as specs. See src/runtime/README.md.
add_library(dq8_runtime STATIC
    ee/guest_stop.cpp
    ee/guest_memory.cpp
    ee/elf_loader.cpp
    ee/context.cpp
    ee/dispatch_table.cpp
    ee/overlay_manager.cpp
    ee/dispatcher.cpp
    ee/ee_runtime.cpp
    ee/game_module.cpp
    kernel/kernel_calls.cpp
    kernel/kernel_hle.cpp
    kernel/kernel_services.cpp
    machine/clock.cpp
    machine/events.cpp
    machine/interrupts.cpp
    machine/mmio.cpp
    platform/dynlib.cpp
)
add_library(dq8::runtime ALIAS dq8_runtime)
# Headers are included as "runtime/<area>/<file>.h"; the module ABI comes from include/ through dq8::core.
target_include_directories(dq8_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(dq8_runtime PUBLIC cxx_std_20)
target_link_libraries(dq8_runtime PUBLIC dq8::core PRIVATE ${CMAKE_DL_LIBS})
dq8_set_warnings(dq8_runtime)
dq8_enable_sanitizers(dq8_runtime)

# M3 session D: the paraLLEl-GS backend (loads dq8-gs-parallel at run time; no Vulkan link) and the library itself.
target_sources(dq8_runtime PRIVATE gs/gs_parallel_backend.cpp)
if(DQ8_BUILD_GS_PARALLEL)
    add_subdirectory(gs/parallel)
    # The build tree's copy: the backend's last place to look, so development builds need no DQ8_GS_PARALLEL.
    set_source_files_properties(gs/gs_parallel_backend.cpp PROPERTIES
        COMPILE_DEFINITIONS "DQ8_GS_PARALLEL_BUILD_PATH=\"$<TARGET_FILE:dq8-gs-parallel>\"")
endif()
