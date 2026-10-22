// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/iop/iop_server.h"
#include "runtime/machine/clock.h"
#include "runtime/machine/events.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The SIF as the EE sees it (architecture §5.2, Docs/milestones/M3-boots.md "Protocol notes"): the four
// hardware registers (MSCOM, SMCOM, MSFLG, SMFLG) and the kernel's software registers, the SIF1 path that
// carries EE transfers into IOP RAM (sceSifSetDma), and the SIF0 path that carries IOP packets back (data
// writes, then the packet in the EE's receive buffer, then a DMAC channel 5 interrupt). The six sceSif*
// kernel services and the two MMIO ranges (registers at 0x1000F200, IOP RAM at 0x1C000000) are registered
// from attach(). Nothing completes instantly: a transfer lands, and a packet is delivered, at a virtual time.
namespace dq8::rt {

class IopMemory;
class MachineServices;
class KernelCallContext;
class IMmioDevice;

// Receives the packets the EE sends to the IOP's command buffer, in landing order.
class ISifCommandSink {
  public:
    virtual ~ISifCommandSink() = default;
    // `packet` is what landed in the command buffer: at least the 16-byte header, at most max_packet_size.
    virtual void on_command(std::span<const std::uint8_t> packet) = 0;
};

class SifLink {
  public:
    // Where the IOP's command buffer lives (published in SMCOM). It sits above the IOP heap, away from IOP
    // address 0, where a sound driver answered by a null server may send data.
    static constexpr std::uint32_t command_buffer = 0x001F0000u;

    explicit SifLink(IopMemory& iop);
    ~SifLink();
    SifLink(const SifLink&) = delete;
    SifLink& operator=(const SifLink&) = delete;

    // Maps the register and IOP RAM ranges and registers the sceSif* kernel services.
    void attach(MachineServices& machine, ISifCommandSink& sink);
    // Power-on: the IOP has booted (SMFLG holds the boot flags, SMCOM the command buffer), nothing is in
    // flight, SIF0 is not armed, the software registers are zero.
    void reset();

    // Register state.
    std::uint32_t mscom() const noexcept { return mscom_; }
    std::uint32_t smcom() const noexcept { return smcom_; }
    std::uint32_t msflg() const noexcept { return msflg_; }
    std::uint32_t smflg() const noexcept { return smflg_; }
    std::uint32_t software_register(std::uint32_t index) const;
    // The IOP side of the handshake.
    void raise_smflg(std::uint32_t bits);
    void set_smcom(std::uint32_t value);

    // The EE buffer IOP packets are delivered to: the address the EE's last init or change-receive command
    // gave; nullopt before the first one.
    void set_ee_receive_buffer(std::uint32_t ee_address);
    std::optional<std::uint32_t> ee_receive_buffer() const;

    // IOP -> EE. `writes` land first, then `packet` (when not empty) in the EE receive buffer, then DMAC
    // channel 5 is raised and SIF0 is disarmed until the EE re-arms it with sceSifSetDChain. An empty packet
    // is a data-only transfer: no interrupt, SIF0 stays armed. Deliveries go in (ready_at, submission)
    // order, never before ready_at and never while SIF0 is not armed. `delivered`, when set, runs once the
    // delivery happened (the interpreted IOP's sceSifDmaStat).
    void send_to_ee(std::vector<std::uint8_t> packet, std::vector<EeWrite> writes, Cycles ready_at,
                    std::function<void(Cycles)> delivered = {});
    // Drops every queued delivery (an IOP reboot).
    void discard_to_ee();

    bool sif0_armed() const noexcept { return sif0_armed_; }
    std::size_t pending_to_ee() const noexcept { return sif0_queue_.size(); }
    std::uint64_t delivered_to_ee() const noexcept { return delivered_; }
    std::uint64_t transfers_to_iop() const noexcept { return transfers_; }

    // sceSifDmaStat's view of a transfer id: > 0 queued, 0 in progress, < 0 done.
    std::int32_t dma_status(std::uint32_t id) const;

    // The register block at 0x1000F200 by byte offset, as EE loads and stores see it: MSFLG stores set bits,
    // SMFLG stores clear bits, SMCOM stores are ignored. nullopt / false for an offset with no register.
    std::optional<std::uint32_t> mmio_read(std::uint32_t offset) const;
    bool mmio_write(std::uint32_t offset, std::uint32_t value);

  private:
    struct Sif0Delivery {
        std::vector<std::uint8_t> packet;
        std::vector<EeWrite> writes;
        std::function<void(Cycles)> delivered;
    };
    struct Landing {
        std::uint32_t dest = 0; // IOP address
        std::vector<std::uint8_t> bytes;
        std::uint32_t attr = 0;
        EventId event = 0;
    };

    void register_services();
    void svc_set_dma(KernelCallContext& call);
    void svc_dma_stat(KernelCallContext& call);
    void svc_set_dchain(KernelCallContext& call);
    void svc_stop_dma(KernelCallContext& call);
    void svc_set_reg(KernelCallContext& call);
    void svc_get_reg(KernelCallContext& call);

    void land(Landing& landing);
    void schedule_sif0();
    void deliver_sif0(Cycles when);
    [[noreturn]] void fail(std::string message) const;

    IopMemory& iop_;
    MachineServices* machine_ = nullptr;
    ISifCommandSink* sink_ = nullptr;
    std::unique_ptr<IMmioDevice> regs_device_;
    std::unique_ptr<IMmioDevice> ram_device_;

    std::uint32_t mscom_ = 0;
    std::uint32_t smcom_ = 0;
    std::uint32_t msflg_ = 0;
    std::uint32_t smflg_ = 0;
    std::uint32_t ctrl_ = 0;
    std::array<std::uint32_t, 32> sregs_{};
    std::optional<std::uint32_t> ee_receive_;

    // SIF1 (EE -> IOP): transfers run back to back; each id remembers when its last transfer lands.
    Cycles sif1_busy_until_ = 0;
    std::uint32_t next_transfer_id_ = 1;
    std::map<std::uint32_t, std::pair<Cycles, Cycles>> transfer_times_; // id -> (start, end), recent ones
    std::map<std::uint64_t, Landing> landings_;                         // by submission number, in flight
    std::uint64_t landing_sequence_ = 0;
    std::uint64_t transfers_ = 0;

    // SIF0 (IOP -> EE).
    bool sif0_armed_ = false;
    Cycles sif0_armed_at_ = 0;
    std::map<std::pair<Cycles, std::uint64_t>, Sif0Delivery> sif0_queue_;
    std::uint64_t sif0_sequence_ = 0;
    EventId sif0_event_ = 0;
    std::uint64_t delivered_ = 0;
};

} // namespace dq8::rt
