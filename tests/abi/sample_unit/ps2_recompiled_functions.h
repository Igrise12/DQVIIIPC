// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's ps2_recompiled_functions.h. A synthetic unit, not game code.
#ifndef PS2_RECOMPILED_FUNCTIONS_H
#define PS2_RECOMPILED_FUNCTIONS_H

#include <cstdint>

struct R5900Context;
class PS2Runtime;

void sample_start_0x200000(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
void sample_helper_0x200060(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
void puts_0x200080(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
void sample_flush_0x2000a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
void sample_skipped_0x2000c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);
void sample_unhandled_0x2000e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);

#endif // PS2_RECOMPILED_FUNCTIONS_H
