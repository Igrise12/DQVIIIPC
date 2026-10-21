# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session C: the sceMpeg HLE, the IPU register stub and the dq8-movie loader. Joins dq8_runtime here.
target_sources(dq8_runtime PRIVATE movie/ipu_regs.cpp)
