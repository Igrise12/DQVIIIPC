// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/machine_services.h"

namespace dq8::rt {

std::string_view to_string(TraceCategory category) noexcept {
    switch (category) {
    case TraceCategory::kernel:
        return "kernel";
    case TraceCategory::sif:
        return "sif";
    case TraceCategory::rpc:
        return "rpc";
    case TraceCategory::dma:
        return "dma";
    case TraceCategory::gs:
        return "gs";
    case TraceCategory::iop:
        return "iop";
    case TraceCategory::spu2:
        return "spu2";
    case TraceCategory::audio:
        return "audio";
    }
    return "unknown";
}

} // namespace dq8::rt
