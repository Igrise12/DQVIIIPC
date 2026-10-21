# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-game: the game executable (start-up, the run's threads and its reports; see src/host/README.md).

find_package(Threads REQUIRED)

# Option parsing, set-up, events and the limiter: a library so that the unit tests reach them too.
add_library(dq8_host STATIC
    options.cpp
    session.cpp
    event_log.cpp
    limiter.cpp
)
target_link_libraries(dq8_host PUBLIC dq8::game dq8::runtime dq8::core)
dq8_set_warnings(dq8_host)
dq8_enable_sanitizers(dq8_host)

# M3 integration guard (TEMPORARY, remove when sessions A, B and C are on main): the machine path
# (run_machine.cpp) links only once dq8_runtime has the machine, the EE devices, the GS pipeline and the IOP.
# Until then dq8-game keeps M2's single-thread run (run_m2.cpp), and run_machine.cpp is still compiled, as an
# object library, against the day-0 contracts.
get_target_property(_dq8_runtime_sources dq8_runtime SOURCES)
set(DQ8_HOST_MACHINE ON)
foreach(_src machine/machine.cpp machine/ee_devices.cpp gs/gs_pipeline.cpp iop/iop.cpp)
    if(NOT _src IN_LIST _dq8_runtime_sources)
        set(DQ8_HOST_MACHINE OFF)
    endif()
endforeach()
message(STATUS "dq8-game: M3 machine path ${DQ8_HOST_MACHINE}")
# tests/unit registers the machine-path tests only when it is there.
set(DQ8_HOST_MACHINE ${DQ8_HOST_MACHINE} PARENT_SCOPE)

if(DQ8_HOST_MACHINE)
    add_executable(dq8-game main.cpp run_machine.cpp)
    set(_dq8_machine_target dq8-game)
else()
    add_executable(dq8-game main.cpp run_m2.cpp)
    add_library(dq8_host_machine_check OBJECT run_machine.cpp)
    target_link_libraries(dq8_host_machine_check PRIVATE dq8_host)
    dq8_set_warnings(dq8_host_machine_check)
    set(_dq8_machine_target dq8_host_machine_check)
endif()
# The SDL3 platform (session D, ADR-0005), when the build has it.
if(TARGET dq8_platform_sdl)
    target_link_libraries(${_dq8_machine_target} PRIVATE dq8_platform_sdl)
    target_compile_definitions(${_dq8_machine_target} PRIVATE DQ8_HOST_HAVE_WINDOW=1)
endif()
target_link_libraries(dq8-game PRIVATE dq8_host Threads::Threads)
dq8_set_warnings(dq8-game)
dq8_enable_sanitizers(dq8-game)

include(GNUInstallDirs)
# Next to ps2_recomp (cmake/Ps2Recomp.cmake installs it to the same directory).
install(TARGETS dq8-game RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT game)
