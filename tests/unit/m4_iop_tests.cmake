# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session A: the interpreted IOP's unit tests (test_iop_*, test_irx_*).
target_sources(dq8_unit_tests PRIVATE
    support/irx_builder.cpp
    test_iop_cpu.cpp
    test_irx_loader.cpp
    test_iop_kernel.cpp
)
