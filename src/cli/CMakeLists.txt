# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
add_executable(dq8ctl main.cpp)
target_link_libraries(dq8ctl PRIVATE dq8::core)
dq8_set_warnings(dq8ctl)
dq8_enable_sanitizers(dq8ctl)
