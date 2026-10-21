# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-gs-replay: a raw GS trace or a PCSX2 GS dump through our paraLLEl-GS backend, to PNG or a window
# (tools/gs/README.md). Contributor tool; not installed.
add_executable(dq8-gs-replay
    dq8_gs_replay.cpp
    gs_stream.cpp
    gs_state_restore.cpp
)
target_link_libraries(dq8-gs-replay PRIVATE dq8::runtime dq8::stb)
# The library this build made is the default; --library or DQ8_GS_PARALLEL choose another.
add_dependencies(dq8-gs-replay dq8_gs_parallel)
target_compile_definitions(dq8-gs-replay PRIVATE DQ8_GS_PARALLEL_BUILD_PATH="$<TARGET_FILE:dq8_gs_parallel>")
if(DQ8_BUILD_WINDOW)
    target_link_libraries(dq8-gs-replay PRIVATE dq8::platform_sdl)
    target_compile_definitions(dq8-gs-replay PRIVATE DQ8_GS_REPLAY_WINDOW=1)
else()
    target_compile_definitions(dq8-gs-replay PRIVATE DQ8_GS_REPLAY_WINDOW=0)
endif()
dq8_set_warnings(dq8-gs-replay)
dq8_enable_sanitizers(dq8-gs-replay)
