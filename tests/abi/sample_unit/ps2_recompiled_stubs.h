// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Hand-written in the shape of the recompiler's ps2_recompiled_stubs.h. A synthetic unit, not game code.
#pragma once

#include <cstdint>
#include "ps2_runtime.h"
#include "ps2_syscalls.h"

void puts_0x200080(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
void sample_flush_0x2000a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
void sample_skipped_0x2000c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
