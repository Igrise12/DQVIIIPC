# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-game: the game executable (start-up and shutdown only; see src/host/README.md).
add_executable(dq8-game main.cpp)
target_link_libraries(dq8-game PRIVATE dq8::game dq8::runtime dq8::core)
dq8_set_warnings(dq8-game)
dq8_enable_sanitizers(dq8-game)
