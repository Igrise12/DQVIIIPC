// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The IOP as native services (architecture §5.2, ADR-0011): the SIF link (SIF registers, SIF0/SIF1 DMA, the
// sceSif* kernel calls), the SIF command and RPC protocol, IOP RAM, the module registry and the RPC servers.
// Nothing executes IOP code. A module load (loadfile RPC) registers the services the game's profile lists for
// that module; an unknown module stops the guest. Implemented by M3 session C.
namespace dq8::rt {

class IDiscSource;
class IInputSource;

// One IOP module the game loads and the native services that stand in for it. Service names are the runtime's
// catalogue: "fileio", "iopheap", "loadfile", "cdvdfsv", "padman.v1", "mcserv.v1", or "null:<sid>" (a server
// that acknowledges every call with zeros and logs it), "null:0x123456" for example.
struct IopModuleSpec {
    std::string name; // file basename without extension, upper case: "PADMAN"
    std::vector<std::string> services;
};

struct IopConfig {
    const IDiscSource* disc = nullptr; // required
    IInputSource* input = nullptr;     // required for padman
    std::vector<IopModuleSpec> modules;
    // The IOP update image the EE resets the IOP with (accepted and not executed; the standard services
    // stay).
    std::string reset_image;
};

class Iop final : public IDevice {
  public:
    explicit Iop(IopConfig config);
    ~Iop() override;
    Iop(const Iop&) = delete;
    Iop& operator=(const Iop&) = delete;

    std::string_view name() const override { return "iop"; }
    void attach(MachineServices& machine) override;
    void reset() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dq8::rt
