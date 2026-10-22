# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
add_executable(dq8ctl main.cpp recomp_commands.cpp setup_commands.cpp settings_commands.cpp)
target_link_libraries(dq8ctl PRIVATE dq8::core)
# When this build also builds the recompiler (cmake/Ps2Recomp.cmake sets DQ8_PS2_RECOMP_EXECUTABLE before
# src/cli is added), `dq8ctl recomp generate` uses it unless --ps2-recomp says otherwise.
if(DQ8_PS2_RECOMP_EXECUTABLE)
    target_compile_definitions(dq8ctl PRIVATE DQ8_DEFAULT_PS2_RECOMP="${DQ8_PS2_RECOMP_EXECUTABLE}")
endif()
# `dq8ctl setup` (the smoke test) and `dq8ctl run` start the dq8-game of this build tree when it is there, else
# the one beside dq8ctl (an install). src/host is added before src/cli.
if(TARGET dq8-game)
    target_compile_definitions(dq8ctl PRIVATE DQ8_DEFAULT_GAME="$<TARGET_FILE:dq8-game>")
endif()
dq8_set_warnings(dq8ctl)
dq8_enable_sanitizers(dq8ctl)
