# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8_platform_sdl: IPlatform over SDL3 (runtime/platform/platform.h, ADR-0005): the window, live input and the M3
# presentation; the audio sink (sdl_audio.h, M4 session B); input profiles and port assignment (M5 session C). Linked only by the host and its tests; the machine
# thread never calls SDL.
add_library(dq8_platform_sdl STATIC
    sdl_audio.cpp
    sdl_bindings.cpp
    sdl_mapping.cpp
    sdl_platform.cpp
    sdl_ports.cpp
)
add_library(dq8::platform_sdl ALIAS dq8_platform_sdl)
target_link_libraries(dq8_platform_sdl PUBLIC dq8::runtime PRIVATE SDL3::SDL3)
dq8_set_warnings(dq8_platform_sdl)
dq8_enable_sanitizers(dq8_platform_sdl)
