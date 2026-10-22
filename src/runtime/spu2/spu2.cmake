# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session B: the SPU2 (and its sound-path helpers). Joins dq8_runtime here.
target_sources(dq8_runtime PRIVATE
    spu2/spu2.cpp
    spu2/spu2_chip.cpp
    spu2/spu2_mix.cpp
)
