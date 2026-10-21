// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/services/services.h"
#include "runtime/machine/machine_services.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace dq8::rt::iop_services {

void unsupported(IopCallContext& call, std::string_view server, std::string_view what) {
    StopInfo info;
    info.reason = StopReason::unsupported;
    info.message = "iop: " + std::string(server) + " (" + hex32(call.sid) + ") function " +
                   std::to_string(call.fno) + ": " + std::string(what);
    call.machine.stop(std::move(info));
    std::abort(); // stop() never returns
}

std::string_view split_device(std::string_view path, std::string& device) {
    device.clear();
    const auto colon = path.find(':');
    if (colon == std::string_view::npos) {
        return path;
    }
    for (const char c : path.substr(0, colon)) {
        device.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return path.substr(colon + 1);
}

} // namespace dq8::rt::iop_services
