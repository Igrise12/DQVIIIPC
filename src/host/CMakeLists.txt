# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-game: the game executable (start-up, the run's threads and its reports; see src/host/README.md).

find_package(Threads REQUIRED)

# Option parsing, set-up, events, the limiter, settings and the status channel: a library so that the unit
# tests reach them too.
add_library(dq8_host STATIC
    options.cpp
    session.cpp
    event_log.cpp
    limiter.cpp
    settings_wiring.cpp
    status_channel.cpp
    display_wiring.cpp
)
target_link_libraries(dq8_host PUBLIC dq8::game dq8::runtime dq8::core)
dq8_set_warnings(dq8_host)
dq8_enable_sanitizers(dq8_host)

# The M4 sessions' host wiring (m4_wiring.h): one file each.
add_executable(dq8-game main.cpp run_machine.cpp audio_wiring.cpp movie_wiring.cpp save_wiring.cpp
    feedback_wiring.cpp)
# The SDL3 platform (session D, ADR-0005), when the build has it.
if(TARGET dq8_platform_sdl)
    target_link_libraries(dq8-game PRIVATE dq8_platform_sdl)
    target_compile_definitions(dq8-game PRIVATE DQ8_HAVE_WINDOW=1)
endif()
target_link_libraries(dq8-game PRIVATE dq8_host Threads::Threads)
dq8_set_warnings(dq8-game)
dq8_enable_sanitizers(dq8-game)

include(GNUInstallDirs)
# Next to ps2_recomp (cmake/Ps2Recomp.cmake installs it to the same directory).
install(TARGETS dq8-game RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT game)
