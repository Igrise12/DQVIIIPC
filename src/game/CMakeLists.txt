# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8_game: everything specific to Dragon Quest VIII, as data-driven as possible (region profiles in, runtime
# specs out). See src/game/README.md.
add_library(dq8_game STATIC
    unit_specs.cpp
)
add_library(dq8::game ALIAS dq8_game)
target_link_libraries(dq8_game PUBLIC dq8::runtime dq8::core)
dq8_set_warnings(dq8_game)
dq8_enable_sanitizers(dq8_game)
