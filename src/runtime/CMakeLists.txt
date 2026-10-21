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
    ee/overrides.cpp
    ee/ee_runtime.cpp
    ee/game_module.cpp
    kernel/kernel_calls.cpp
    kernel/kernel_services.cpp
    kernel/fiber_stack.cpp
    kernel/scheduler.cpp
    kernel/kernel_objects.cpp
    kernel/services_thread.cpp
    kernel/services_sync.cpp
    kernel/services_interrupt.cpp
    kernel/services_system.cpp
    machine/clock.cpp
    machine/events.cpp
    machine/interrupts.cpp
    machine/machine.cpp
    machine/machine_services.cpp
    machine/mmio.cpp
    machine/intc_regs.cpp
    machine/ee_timers.cpp
    machine/ee_devices.cpp
    dma/gif.cpp
    dma/vif.cpp
    dma/dmac.cpp
    gs/gs_priv_device.cpp
    gs/gs_pipeline.cpp
    gs/gs_backends.cpp
    gs/png_writer.cpp
    vu/vu_core.cpp
    vu/vu_lower.cpp
    vu/vu_upper.cpp
    vu/vu_units.cpp
    io/input_script.cpp
    io/input_live.cpp
    io/input_record.cpp
    platform/dynlib.cpp
    io/iso_disc_source.cpp
    iop/iop.cpp
    iop/iop_memory.cpp
    iop/iop_modules.cpp
    iop/iop_rpc.cpp
    iop/sif_link.cpp
    iop/services/cdvdfsv.cpp
    iop/services/common.cpp
    iop/services/fileio.cpp
    iop/services/iopheap.cpp
    iop/services/loadfile.cpp
    iop/services/mcserv.cpp
    iop/services/null_server.cpp
    iop/services/padman.cpp
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
find_package(Threads REQUIRED)
target_link_libraries(dq8_runtime PUBLIC dq8::core PRIVATE dq8::stb Threads::Threads ${CMAKE_DL_LIBS})
dq8_set_warnings(dq8_runtime)
dq8_enable_sanitizers(dq8_runtime)

# M3 session D: the paraLLEl-GS backend (loads dq8-gs-parallel at run time; no Vulkan link), the library itself, and
# the SDL3 platform (dq8_platform_sdl, linked only by the host).
target_sources(dq8_runtime PRIVATE gs/gs_parallel_backend.cpp)
if(DQ8_BUILD_GS_PARALLEL)
    add_subdirectory(gs/parallel)
    # The build tree's copy: the backend's last place to look, so development builds need no DQ8_GS_PARALLEL.
    set_source_files_properties(gs/gs_parallel_backend.cpp PROPERTIES
        COMPILE_DEFINITIONS "DQ8_GS_PARALLEL_BUILD_PATH=\"$<TARGET_FILE:dq8_gs_parallel>\"")
endif()
if(DQ8_BUILD_WINDOW)
    add_subdirectory(platform/sdl)
endif()
