# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session B: the launcher's view-models (test_launcher_*), in their own executable since the launcher never
# links the runtime.
if(TARGET dq8-launcher)
    add_test(NAME launcher.version COMMAND dq8-launcher --version)
    set_tests_properties(launcher.version PROPERTIES PASS_REGULAR_EXPRESSION "^dq8-launcher " LABELS unit)
    # The whole Dear ImGui over SDL3 stack for a few frames, on SDL's dummy video driver (no display needed).
    add_test(NAME launcher.dummy_frames COMMAND dq8-launcher --frames 3)
    set_tests_properties(launcher.dummy_frames PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy" LABELS unit)
endif()
