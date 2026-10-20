// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// ps2_stubs::<name> for every name in PS2_STUB_LIST (ps2_call_list.h), plus TODO and TODO_NAMED. Generated
// code calls these from the body of a function the recompiler replaced: a map name found in PS2_STUB_LIST, a
// "Name@0xADDR" binding, or (TODO_NAMED) a function the config skips. Each call goes to the host by name, as
// DQ8_CALL_STUB or DQ8_CALL_TODO (ps2_named_call.h says how). Upstream's header declares the same names and
// links them against its own runtime; ours are header-only, so a game module has no undefined references
// (game_module.h).
#pragma once

#include "ps2_call_list.h"
#include "ps2_named_call.h"
#include "ps2_runtime.h"

#include <cstdint>

namespace ps2_stubs {

#define DQ8_PS2_STUB_FORWARDER(name) using name = dq8_named_call<DQ8_CALL_STUB, #name>;
PS2_STUB_LIST(DQ8_PS2_STUB_FORWARDER)
#undef DQ8_PS2_STUB_FORWARDER

// A call the recompiler had nothing for. TODO has no name to report; TODO_NAMED carries the function's map
// name.
inline void TODO_NAMED(const char* name, std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    runtime->api->call_named(runtime->host, DQ8_CALL_TODO, name, rdram, ctx);
}
inline void TODO(std::uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    TODO_NAMED("TODO", rdram, ctx, runtime);
}

} // namespace ps2_stubs
