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
    platform/dynlib.cpp
)
add_library(dq8::runtime ALIAS dq8_runtime)
# Headers are included as "runtime/<area>/<file>.h"; the module ABI comes from include/ through dq8::core.
target_include_directories(dq8_runtime PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(dq8_runtime PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(dq8_runtime PUBLIC dq8::core PRIVATE dq8::stb Threads::Threads ${CMAKE_DL_LIBS})
dq8_set_warnings(dq8_runtime)
dq8_enable_sanitizers(dq8_runtime)
