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
    platform/dynlib.cpp
)
add_library(dq8::runtime ALIAS dq8_runtime)
# Headers are included as "runtime/<area>/<file>.h"; the module ABI comes from include/ through dq8::core.
target_include_directories(dq8_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(dq8_runtime PUBLIC cxx_std_20)
target_link_libraries(dq8_runtime PUBLIC dq8::core PRIVATE ${CMAKE_DL_LIBS})
dq8_set_warnings(dq8_runtime)
dq8_enable_sanitizers(dq8_runtime)
