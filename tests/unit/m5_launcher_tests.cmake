# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session B: the launcher. Its view-models are tested in their own executable (test_launcher_*), since the
# launcher never links the runtime; the window itself runs on SDL's dummy video driver.
if(TARGET dq8-launcher)
    add_test(NAME launcher.version COMMAND dq8-launcher --version)
    set_tests_properties(launcher.version PROPERTIES PASS_REGULAR_EXPRESSION "^dq8-launcher " LABELS unit)
    # The whole Dear ImGui over SDL3 stack for a few frames, on SDL's dummy video driver (no display needed).
    add_test(NAME launcher.dummy_frames COMMAND dq8-launcher --frames 3)
    set_tests_properties(launcher.dummy_frames PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy" LABELS unit)
    # Every page drawn over the --preview stand-ins (a simulated setup and game; nothing written). With --frames
    # the preview starts the page's work by itself, so the running states are drawn too; 24 frames on SDL's
    # software renderer are about two seconds, enough for the simulated setup and game to report.
    foreach(_page setup play settings controllers diagnostics about)
        add_test(NAME launcher.preview_${_page} COMMAND dq8-launcher --preview --page ${_page} --frames 24)
        set_tests_properties(launcher.preview_${_page} PROPERTIES
            ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy" LABELS unit)
    endforeach()
endif()

if(TARGET dq8_launcher_model)
    # A stand-in for dq8-game that prints status lines (the supervisor's end-to-end test).
    add_executable(dq8_launcher_fake_game support/launcher_fake_game.cpp)
    dq8_set_warnings(dq8_launcher_fake_game)
    dq8_enable_sanitizers(dq8_launcher_fake_game)

    add_executable(dq8_launcher_tests
        test_launcher_controllers.cpp
        test_launcher_library.cpp
        test_launcher_misc.cpp
        test_launcher_session.cpp
        test_launcher_settings.cpp
        test_launcher_status.cpp
        test_launcher_wizard.cpp
    )
    target_link_libraries(dq8_launcher_tests PRIVATE dq8_launcher_model toml11::toml11 Catch2::Catch2WithMain)
    target_compile_definitions(dq8_launcher_tests PRIVATE
        DQ8_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        DQ8_LAUNCHER_FAKE_GAME="$<TARGET_FILE:dq8_launcher_fake_game>")
    add_dependencies(dq8_launcher_tests dq8_launcher_fake_game)
    dq8_set_warnings(dq8_launcher_tests)
    dq8_enable_sanitizers(dq8_launcher_tests)
    include(Catch)
    catch_discover_tests(dq8_launcher_tests PROPERTIES LABELS unit)
endif()
