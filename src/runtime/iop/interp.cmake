# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M4 session A: the interpreted IOP (iop/cpu, iop/loader, iop/kernel, iop/hw, iop/iop_exec). Sources join
# dq8_runtime here so that no other session edits src/runtime/CMakeLists.txt.
target_sources(dq8_runtime PRIVATE
    iop/cpu/iop_cpu.cpp
    iop/hw/iop_hardware.cpp
    iop/loader/irx_image.cpp
    iop/loader/iop_libraries.cpp
    iop/kernel/iop_kernel.cpp
    iop/kernel/iop_printf.cpp
    iop/kernel/hle_table.cpp
    iop/kernel/hle_intrman.cpp
    iop/kernel/hle_system.cpp
    iop/kernel/hle_thread.cpp
    iop/kernel/hle_timrman.cpp
    iop/kernel/hle_sif.cpp
    iop/iop_exec.cpp
)
