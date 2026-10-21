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
    kernel/fiber_stack.cpp
    machine/clock.cpp
    machine/events.cpp
    machine/interrupts.cpp
    machine/machine_services.cpp
    machine/mmio.cpp
    platform/dynlib.cpp
)
# Exactly one fiber backend (ADR-0007, DQ8_FIBER_BACKEND in cmake/Dq8Options.cmake).
if(DQ8_FIBER_BACKEND STREQUAL "minicoro")
    target_sources(dq8_runtime PRIVATE kernel/fiber_minicoro.cpp)
    target_link_libraries(dq8_runtime PRIVATE dq8::minicoro)
    # minicoro's own ASan annotation misses the switch back to the scheduler's stack: build the backend
    # without ASan and let it annotate every switch itself (fiber_minicoro.cpp).
    if(DQ8_SANITIZERS MATCHES "address" AND NOT MSVC)
        set_source_files_properties(kernel/fiber_minicoro.cpp PROPERTIES
            COMPILE_OPTIONS "-fno-sanitize=address"
            COMPILE_DEFINITIONS "DQ8_FIBER_ASAN=1")
    endif()
elseif(DQ8_FIBER_BACKEND STREQUAL "ucontext")
    if(WIN32)
        message(FATAL_ERROR "DQ8_FIBER_BACKEND=ucontext needs a POSIX host")
    endif()
    target_sources(dq8_runtime PRIVATE kernel/fiber_ucontext.cpp)
    if(APPLE)
        target_compile_definitions(dq8_runtime PRIVATE _XOPEN_SOURCE=700)
    endif()
elseif(DQ8_FIBER_BACKEND STREQUAL "win32")
    if(NOT WIN32)
        message(FATAL_ERROR "DQ8_FIBER_BACKEND=win32 needs Windows")
    endif()
    target_sources(dq8_runtime PRIVATE kernel/fiber_win32.cpp)
else()
    message(FATAL_ERROR "DQ8_FIBER_BACKEND must be minicoro, ucontext or win32, not '${DQ8_FIBER_BACKEND}'")
endif()
add_library(dq8::runtime ALIAS dq8_runtime)
# Headers are included as "runtime/<area>/<file>.h"; the module ABI comes from include/ through dq8::core.
target_include_directories(dq8_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(dq8_runtime PUBLIC cxx_std_20)
target_link_libraries(dq8_runtime PUBLIC dq8::core PRIVATE ${CMAKE_DL_LIBS})
dq8_set_warnings(dq8_runtime)
dq8_enable_sanitizers(dq8_runtime)
