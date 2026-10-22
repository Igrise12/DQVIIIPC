# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-launcher: the GUI front end (architecture.md §11, ADR-0010). It links libdq8core and the UI libraries only,
# never the runtime, and starts dq8-game as a separate process. M5 day 0 is a stub window; M5 session B owns this
# directory and builds the wizard, Play, Settings, Controllers, Diagnostics and About on it.

add_executable(dq8-launcher main.cpp)
target_link_libraries(dq8-launcher PRIVATE dq8::core dq8::imgui dq8::nlohmann_json SDL3::SDL3)
dq8_set_warnings(dq8-launcher)
dq8_enable_sanitizers(dq8-launcher)

include(GNUInstallDirs)
install(TARGETS dq8-launcher RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT launcher)
