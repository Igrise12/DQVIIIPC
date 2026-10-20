// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's register_functions.cpp: one slot per 4 bytes from the lowest to the
// highest entry, filled by a static initialiser, with resume entries (jal+8, syscall+4, backward-branch targets)
// pointing at the function that owns them. A synthetic unit, not game code.
#include "ps2_runtime.h"
#include <ps2_recompiled_functions.h>
#include "ps2_stubs.h"
#include <ps2_recompiled_stubs.h>
#include "ps2_syscalls.h"

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x200000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x2000e4u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 57u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[57u] = {};

namespace {
struct GeneratedFunctionTableInitializer {
    GeneratedFunctionTableInitializer() {
        g_ps2RecompiledFunctionTable[0] = sample_start_0x200000; // 0x200000
        g_ps2RecompiledFunctionTable[4] = sample_start_0x200000; // 0x200010
        g_ps2RecompiledFunctionTable[11] = sample_start_0x200000; // 0x20002c
        g_ps2RecompiledFunctionTable[13] = sample_start_0x200000; // 0x200034
        g_ps2RecompiledFunctionTable[15] = sample_start_0x200000; // 0x20003c
        g_ps2RecompiledFunctionTable[17] = sample_start_0x200000; // 0x200044
        g_ps2RecompiledFunctionTable[24] = sample_helper_0x200060; // 0x200060
        g_ps2RecompiledFunctionTable[26] = sample_helper_0x200060; // 0x200068
        g_ps2RecompiledFunctionTable[32] = puts_0x200080; // 0x200080
        g_ps2RecompiledFunctionTable[40] = sample_flush_0x2000a0; // 0x2000a0
        g_ps2RecompiledFunctionTable[48] = sample_skipped_0x2000c0; // 0x2000c0
        g_ps2RecompiledFunctionTable[56] = sample_unhandled_0x2000e0; // 0x2000e0
    }
};
static const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;
}
