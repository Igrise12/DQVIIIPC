// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/machine_services.h"

#include <memory>
#include <vector>

namespace dq8::gs {
class IGsSink;
}

// The EE-side peripherals as devices (architecture §5.3-§5.5): EE timers, the INTC and DMAC registers, the
// DMAC channels, VIF0 and VIF1, the GIF, VU0 and VU1 memory with the VU1 interpreter, the GS privileged
// registers and the GS kernel calls (SetGsCrt, GsGetIMR/GsPutIMR, SetVSyncFlag, GetGsDxDyOffset), the SIO
// TTY. Implemented by M3 session B.
namespace dq8::rt {

struct EeDevicesConfig {
    gs::IGsSink* gs = nullptr; // required: where GIF packets and vsyncs go
};

// In attach order.
std::vector<std::unique_ptr<IDevice>> make_ee_devices(const EeDevicesConfig& config);

} // namespace dq8::rt
