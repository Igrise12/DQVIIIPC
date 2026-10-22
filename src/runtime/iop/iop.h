// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/hw/iop_hw.h"
#include "runtime/machine/machine_services.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The IOP as native services (architecture §5.2, ADR-0011): the SIF link (SIF registers, SIF0/SIF1 DMA, the
// sceSif* kernel calls), the SIF command and RPC protocol, IOP RAM, the module registry and the RPC servers.
// A module load (loadfile RPC) registers the services the game's profile lists for that module; an unknown
// module stops the guest. Implemented by M3 session C. M4 (ADR-0014, session A): a module whose profile row
// is "interpret" is loaded from the disc and run on the interpreted IOP (iop_exec.h); the loadfile reply
// waits for its start function.
namespace dq8::rt {

class IDiscSource;
class IInputSource;
class IMemoryCardStore;
class IPadFeedbackSink;
class VirtualRtc;

// One IOP module the game loads and the native services that stand in for it. Service names are the runtime's
// catalogue: "fileio", "iopheap", "loadfile", "cdvdfsv", "padman.v1", "mcserv.v1", or "null:<sid>" (a server
// that acknowledges every call with zeros and logs it), "null:0x123456" for example; or "interpret" alone
// (M4: the module's own code runs).
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
    // M4 (Docs/milestones/M4-playable.md, "Contracts"):
    // IOP-side devices for the interpreted modules (the SPU2), mapped on the IOP bus (session A).
    std::vector<IopDeviceMapping> devices;
    // An interpreted module that imports an HLE function nobody implemented stops at load instead of at its
    // first call.
    bool strict_imports = false;
    // The memory cards in ports 0 and 1 (null: no card), the virtual clock for timestamps, and where the pad
    // service reports actuator changes (session D).
    IMemoryCardStore* cards[2] = {nullptr, nullptr};
    const VirtualRtc* rtc = nullptr;
    IPadFeedbackSink* pad_feedback = nullptr;
};

class IopExec;

class Iop final : public IDevice {
  public:
    explicit Iop(IopConfig config);
    ~Iop() override;
    Iop(const Iop&) = delete;
    Iop& operator=(const Iop&) = delete;

    std::string_view name() const override { return "iop"; }
    void attach(MachineServices& machine) override;
    void reset() override;

    // The 2 MiB of IOP RAM (--dump-iop-ram-at).
    std::span<const std::uint8_t> ram() const;
    // The interpreted IOP (after attach).
    IopExec& interpreter();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dq8::rt
