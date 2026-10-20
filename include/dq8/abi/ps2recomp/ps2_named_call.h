// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// The by-name calls behind ps2_syscalls::<name> and ps2_stubs::<name> (ps2_syscalls.h, ps2_stubs.h).
//
// Each name in PS2_SYSCALL_LIST and PS2_STUB_LIST becomes an alias of dq8_named_call<kind, "name">, a type
// whose constructor makes the call. The emitter's `ps2_stubs::name(rdram, ctx, runtime);` therefore
// constructs a temporary, which calls runtime->api->call_named(runtime->host, kind, "name", rdram, ctx). An
// alias and not an inline function because the lists may repeat a name (PS2_STUB_LIST at eb8688b2 names
// sceSifLoadElfPart twice): upstream only declares the functions, where a repeat is harmless, and two
// definitions of one inline function are an error, whereas two aliases of the same type are allowed.
#pragma once

#include "ps2_runtime.h"

#include <cstddef>
#include <cstdint>

// A string literal as a template argument.
template <std::size_t N> struct dq8_call_name {
    char text[N];
    constexpr dq8_call_name(const char (&s)[N]) noexcept : text{} {
        for (std::size_t i = 0; i < N; ++i) {
            text[i] = s[i];
        }
    }
};

template <std::uint32_t Kind, dq8_call_name Name> struct dq8_named_call {
    dq8_named_call(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
        runtime->api->call_named(runtime->host, Kind, Name.text, rdram, ctx);
    }
};
