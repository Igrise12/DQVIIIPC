# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-launcher: the GUI front end (architecture.md §11, ADR-0010). It links libdq8core and the UI libraries only,
# never the runtime, and starts dq8-game and dq8ctl as separate processes.

find_package(Threads REQUIRED)

# The view-models: what the launcher decides, with no window, SDL or Dear ImGui, so the unit tests
# (tests/unit/m5_launcher_tests.cmake) reach all of it.
add_library(dq8_launcher_model STATIC
    model/controllers.cpp
    model/core_backends.cpp
    model/diagnostics.cpp
    model/environment.cpp
    model/errors.cpp
    model/executor.cpp
    model/game_session.cpp
    model/launcher_state.cpp
    model/library.cpp
    model/log_buffer.cpp
    model/markdown.cpp
    model/preview.cpp
    model/settings_model.cpp
    model/setup_progress.cpp
    model/status.cpp
    model/wizard.cpp
)
target_include_directories(dq8_launcher_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# core's internal platform headers (paths, atomic file writes), as dq8-game uses them.
target_include_directories(dq8_launcher_model PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dq8_launcher_model PUBLIC dq8::core Threads::Threads PRIVATE dq8::nlohmann_json)
dq8_set_warnings(dq8_launcher_model)
dq8_enable_sanitizers(dq8_launcher_model)

# The bundled font (data/fonts, SIL OFL 1.1; THIRD_PARTY_NOTICES.md), compiled in so the launcher always has it.
set(_dq8_launcher_font "${PROJECT_SOURCE_DIR}/data/fonts/NotoSans-Regular.ttf")
set(_dq8_launcher_font_cpp "${CMAKE_CURRENT_BINARY_DIR}/launcher_font.cpp")
add_custom_command(
    OUTPUT "${_dq8_launcher_font_cpp}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_dq8_launcher_font}" "-DOUTPUT=${_dq8_launcher_font_cpp}"
            -DSYMBOL=launcher_font -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_file.cmake"
    DEPENDS "${_dq8_launcher_font}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_file.cmake"
    COMMENT "Embedding the launcher's font"
    VERBATIM)

add_executable(dq8-launcher
    main.cpp
    ui/app.cpp
    ui/page_about.cpp
    ui/page_controllers.cpp
    ui/page_diagnostics.cpp
    ui/page_play.cpp
    ui/page_settings.cpp
    ui/page_setup.cpp
    ui/sdl_input.cpp
    ui/widgets.cpp
    "${_dq8_launcher_font_cpp}"
)
target_include_directories(dq8-launcher PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(dq8-launcher PRIVATE dq8_launcher_model dq8::imgui SDL3::SDL3)
dq8_set_warnings(dq8-launcher)
dq8_enable_sanitizers(dq8-launcher)

include(GNUInstallDirs)
install(TARGETS dq8-launcher RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT launcher)
