// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/sif_link.h"

#include "runtime/ee/guest_memory.h"
#include "runtime/ee/guest_stop.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/sif_protocol.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/interrupts.h"
#include "runtime/machine/machine_services.h"
#include "runtime/machine/mmio.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace dq8::rt {

namespace {

// The address sceSifDmaStat polls are reported under (the SIF1 channel's control register), so the spin
// detector can tell a transfer wait from other polling.
constexpr std::uint32_t sif1_poll_address = 0x1000C400u;
// How many recent transfer ids sceSifDmaStat can still tell apart; older ones are long done.
constexpr std::size_t transfer_history = 256;

// The MMIO offset of a hardware register number (1..4): MSCOM, SMCOM, MSFLG, SMFLG are 16 bytes apart.
std::uint32_t register_offset(std::uint32_t reg) {
    return (reg - sif::reg_mscom) * 0x10u;
}

Cycles transfer_cost(const CostModel& cost, std::uint64_t bytes) {
    const std::uint64_t qwords = std::max<std::uint64_t>(1, (bytes + 15) / 16);
    return qwords * cost.dma_per_qword;
}

// The SIF registers at 0x1000F200 as the EE's loads and stores see them.
class SifRegisterDevice final : public IMmioDevice {
  public:
    SifRegisterDevice(SifLink& link, MachineServices& machine) : link_(link), machine_(machine) {}

    std::string_view name() const override { return "sif"; }

    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext& at) override {
        const auto value = width == 4 ? link_.mmio_read(offset) : std::nullopt;
        if (!value) {
            return false;
        }
        machine_.note_poll(at.pc, sif::mmio_base + offset, *value);
        std::memcpy(out, &*value, 4);
        return true;
    }

    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext&) override {
        if (width != 4) {
            return false;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, in, 4);
        if (!link_.mmio_write(offset, value)) {
            return false;
        }
        machine_.note_activity();
        return true;
    }

  private:
    SifLink& link_;
    MachineServices& machine_;
};

// IOP RAM as the EE sees it at 0x1C000000 (mirrors beyond 2 MiB are not mapped).
class IopRamDevice final : public IMmioDevice {
  public:
    explicit IopRamDevice(IopMemory& iop) : iop_(iop) {}
    std::string_view name() const override { return "iop_ram"; }
    bool read(std::uint32_t offset, std::uint32_t width, std::uint8_t* out, const MmioContext&) override {
        iop_.read(offset, {out, width});
        return true;
    }
    bool write(std::uint32_t offset, std::uint32_t width, const std::uint8_t* in,
               const MmioContext&) override {
        iop_.write(offset, {in, width});
        return true;
    }

  private:
    IopMemory& iop_;
};

} // namespace

SifLink::SifLink(IopMemory& iop) : iop_(iop) {}

SifLink::~SifLink() = default;

void SifLink::attach(MachineServices& machine, ISifCommandSink& sink) {
    machine_ = &machine;
    sink_ = &sink;
    regs_device_ = std::make_unique<SifRegisterDevice>(*this, machine);
    ram_device_ = std::make_unique<IopRamDevice>(iop_);
    machine.mmio().map(sif::mmio_base, sif::mmio_size, *regs_device_);
    machine.mmio().map(sif::iop_ram_phys, IopMemory::size, *ram_device_);
    register_services();
}

void SifLink::reset() {
    if (machine_ != nullptr) {
        for (auto& [key, landing] : landings_) {
            machine_->events().cancel(landing.event);
        }
        if (sif0_event_ != 0) {
            machine_->events().cancel(sif0_event_);
        }
    }
    landings_.clear();
    sif0_event_ = 0;
    mscom_ = 0;
    msflg_ = 0;
    ctrl_ = 0;
    smcom_ = command_buffer;
    smflg_ = sif::boot_flags;
    sregs_.fill(0);
    ee_receive_.reset();
    sif1_busy_until_ = 0;
    next_transfer_id_ = 1;
    transfer_times_.clear();
    transfers_ = 0;
    sif0_armed_ = false;
    sif0_armed_at_ = 0;
    sif0_queue_.clear();
    sif0_sequence_ = 0;
    delivered_ = 0;
}

std::uint32_t SifLink::software_register(std::uint32_t index) const {
    return index < sregs_.size() ? sregs_[index] : 0;
}

void SifLink::raise_smflg(std::uint32_t bits) {
    smflg_ |= bits;
    if (machine_ != nullptr) {
        machine_->note_activity();
    }
}

void SifLink::set_smcom(std::uint32_t value) {
    smcom_ = value;
    if (machine_ != nullptr) {
        machine_->note_activity();
    }
}

void SifLink::set_ee_receive_buffer(std::uint32_t ee_address) {
    ee_receive_ = ee_address;
}

std::optional<std::uint32_t> SifLink::ee_receive_buffer() const {
    if (ee_receive_ && *ee_receive_ != 0) {
        return ee_receive_;
    }
    return std::nullopt;
}

std::int32_t SifLink::dma_status(std::uint32_t id) const {
    const auto it = transfer_times_.find(id);
    if (it == transfer_times_.end() || machine_ == nullptr) {
        return -1;
    }
    const Cycles now = machine_->clock().now();
    if (now < it->second.first) {
        return 1;
    }
    return now < it->second.second ? 0 : -1;
}

void SifLink::register_services() {
    auto add = [this](const char* name, void (SifLink::*fn)(KernelCallContext&), std::uint32_t flags) {
        KernelServiceInfo info;
        info.name = name;
        info.fn = [this, fn](KernelCallContext& call) { (this->*fn)(call); };
        info.flags = flags | kernel_service_interrupt_ok;
        info.owner = "iop";
        machine_->kernel().add(std::move(info));
    };
    add("sceSifSetDma", &SifLink::svc_set_dma, 0);
    add("sceSifDmaStat", &SifLink::svc_dma_stat, 0);
    add("sceSifSetDChain", &SifLink::svc_set_dchain, 0);
    add("sceSifStopDma", &SifLink::svc_stop_dma, 0);
    add("sceSifSetReg", &SifLink::svc_set_reg, 0);
    add("sceSifGetReg", &SifLink::svc_get_reg, 0);
}

// sceSifSetDma(descriptors, count): queue EE -> IOP transfers; returns a non-zero id for sceSifDmaStat.
void SifLink::svc_set_dma(KernelCallContext& call) {
    const std::uint32_t table = call.arg(0);
    const std::uint32_t count = call.arg(1);
    if (count == 0 || count > sif::dma_max_descriptors) {
        call.unimplemented("a transfer list of " + std::to_string(count) + " descriptors");
    }
    GuestMemory& mem = machine_->memory();
    const CostModel& cost = machine_->cost();
    Cycles start = std::max(machine_->clock().now(), sif1_busy_until_);
    const Cycles first_start = start;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t desc[sif::dma_desc_size];
        try {
            mem.read_bytes(table + i * sif::dma_desc_size, desc);
        } catch (const std::out_of_range&) {
            fail("sceSifSetDma: descriptor " + std::to_string(i) + " at " + hex32(table) + " is not in RAM");
        }
        const std::span<const std::uint8_t> d{desc};
        const std::uint32_t src = sif::load32(d, sif::dma_desc_src);
        const std::uint32_t dest = sif::load32(d, sif::dma_desc_dest);
        const std::uint32_t size = sif::load32(d, sif::dma_desc_size_field);
        const std::uint32_t attr = sif::load32(d, sif::dma_desc_attr);
        // The IOP side receives whole words. IOP addresses lose their segment bits, and IOP RAM mirrors every
        // 2 MiB up to 8 MiB.
        const std::uint32_t bytes = (size + 3u) & ~3u;
        const std::uint32_t physical = dest & 0x1FFFFFFFu;
        if (physical >= 0x00800000u) {
            fail("sceSifSetDma: destination " + hex32(dest) + " is not IOP RAM");
        }
        const std::uint32_t iop_addr = physical & (IopMemory::size - 1);
        if (bytes > IopMemory::size - iop_addr) {
            fail("sceSifSetDma: " + std::to_string(size) + " bytes to " + hex32(dest) + " run past IOP RAM");
        }
        Landing landing;
        landing.dest = iop_addr;
        landing.attr = attr;
        landing.bytes.resize(bytes);
        try {
            mem.read_bytes(src, landing.bytes);
        } catch (const std::out_of_range&) {
            fail("sceSifSetDma: source " + hex32(src) + " + " + std::to_string(bytes) + " is not in RAM");
        }
        const Cycles end = start + transfer_cost(cost, bytes);
        if (machine_->tracing(TraceCategory::sif)) {
            machine_->trace(TraceCategory::sif, "SetDma " + hex32(src) + " -> IOP " + hex32(iop_addr) + " +" +
                                                    std::to_string(size) + " attr " + hex32(attr));
        }
        const std::uint64_t key = ++landing_sequence_;
        landing.event = machine_->events().schedule(end, "sif1_land", [this, key](Cycles) {
            const auto it = landings_.find(key);
            if (it == landings_.end()) {
                return;
            }
            Landing l = std::move(it->second);
            landings_.erase(it);
            land(l);
        });
        landings_.emplace(key, std::move(landing));
        start = end;
    }
    sif1_busy_until_ = start;

    const std::uint32_t id = next_transfer_id_++;
    if (next_transfer_id_ == 0) {
        next_transfer_id_ = 1;
    }
    transfer_times_[id] = {first_start, start};
    while (transfer_times_.size() > transfer_history) {
        transfer_times_.erase(transfer_times_.begin());
    }
    machine_->note_activity();
    call.ret_u32(id);
}

void SifLink::svc_dma_stat(KernelCallContext& call) {
    const std::int32_t status = dma_status(call.arg(0));
    machine_->note_poll(call.ctx().pc, sif1_poll_address, static_cast<std::uint32_t>(status));
    call.ret(status);
}

// sceSifSetDChain: the EE (re)arms SIF0 to receive the next IOP packet.
void SifLink::svc_set_dchain(KernelCallContext& call) {
    if (!sif0_armed_) {
        machine_->trace(TraceCategory::sif, "SIF0 armed");
        sif0_armed_ = true;
        sif0_armed_at_ = machine_->clock().now();
        schedule_sif0();
    }
    machine_->note_activity();
    call.ret(0);
}

// sceSifStopDma: the EE stops SIF0 (before an IOP reset). Queued deliveries wait for the next arming.
void SifLink::svc_stop_dma(KernelCallContext& call) {
    machine_->trace(TraceCategory::sif, "SIF0 stopped");
    sif0_armed_ = false;
    schedule_sif0();
    machine_->note_activity();
    call.ret(0);
}

void SifLink::svc_set_reg(KernelCallContext& call) {
    const std::uint32_t reg = call.arg(0);
    const std::uint32_t value = call.arg(1);
    if (machine_->tracing(TraceCategory::sif)) {
        machine_->trace(TraceCategory::sif, "SetReg " + hex32(reg) + " = " + hex32(value));
    }
    if ((reg & sif::reg_software) != 0) {
        const std::uint32_t index = reg & ~sif::reg_software;
        if (index >= sregs_.size()) {
            call.unimplemented("software register " + std::to_string(index));
        }
        sregs_[index] = value;
    } else if (reg >= sif::reg_mscom && reg <= sif::reg_smflg) {
        mmio_write(register_offset(reg), value);
    } else {
        call.unimplemented("SIF register number " + hex32(reg));
    }
    machine_->note_activity();
    call.ret(0);
}

void SifLink::svc_get_reg(KernelCallContext& call) {
    const std::uint32_t reg = call.arg(0);
    std::uint32_t value = 0;
    std::uint32_t poll_address = reg;
    if ((reg & sif::reg_software) != 0) {
        const std::uint32_t index = reg & ~sif::reg_software;
        if (index >= sregs_.size()) {
            call.unimplemented("software register " + std::to_string(index));
        }
        value = sregs_[index];
    } else if (reg >= sif::reg_mscom && reg <= sif::reg_smflg) {
        value = *mmio_read(register_offset(reg));
        poll_address = sif::mmio_base + register_offset(reg);
    } else {
        call.unimplemented("SIF register number " + hex32(reg));
    }
    machine_->note_poll(call.ctx().pc, poll_address, value);
    call.ret_u32(value);
}

std::optional<std::uint32_t> SifLink::mmio_read(std::uint32_t offset) const {
    switch (offset) {
    case sif::mmio_mscom:
        return mscom_;
    case sif::mmio_smcom:
        return smcom_;
    case sif::mmio_msflg:
        return msflg_;
    case sif::mmio_smflg:
        return smflg_;
    case sif::mmio_ctrl:
        return ctrl_;
    case sif::mmio_bd6:
        return 0u;
    default:
        return std::nullopt;
    }
}

bool SifLink::mmio_write(std::uint32_t offset, std::uint32_t value) {
    switch (offset) {
    case sif::mmio_mscom:
        mscom_ = value;
        return true;
    case sif::mmio_smcom:
        return true; // the IOP's mailbox: an EE store does not reach it
    case sif::mmio_msflg:
        msflg_ |= value;
        return true;
    case sif::mmio_smflg:
        smflg_ &= ~value;
        return true;
    case sif::mmio_ctrl:
        ctrl_ = (value & 0x100u) != 0 ? (ctrl_ | 0x100u) : (ctrl_ & ~0x100u);
        return true;
    case sif::mmio_bd6:
        return true;
    default:
        return false;
    }
}

void SifLink::land(Landing& landing) {
    iop_.write(landing.dest, landing.bytes);
    ++transfers_;
    machine_->note_activity();
    if (landing.dest == command_buffer && sink_ != nullptr) {
        const std::size_t n = std::min<std::size_t>(landing.bytes.size(), sif::max_packet_size);
        if (n < sif::header_size) {
            fail("a " + std::to_string(landing.bytes.size()) +
                 "-byte transfer to the IOP command buffer is "
                 "shorter than a command header");
        }
        const auto packet = std::span<const std::uint8_t>(landing.bytes).first(n);
        if (machine_->tracing(TraceCategory::sif)) {
            const std::uint32_t cid = sif::load32(packet, sif::hdr_cid);
            machine_->trace(TraceCategory::sif,
                            "IOP receives " + hex32(cid) + " (" + std::string(sif::command_name(cid)) + ")");
        }
        sink_->on_command(packet);
    }
}

void SifLink::send_to_ee(std::vector<std::uint8_t> packet, std::vector<EeWrite> writes, Cycles ready_at) {
    sif0_queue_.emplace(std::pair{ready_at, sif0_sequence_++},
                        Sif0Delivery{std::move(packet), std::move(writes)});
    schedule_sif0();
}

void SifLink::discard_to_ee() {
    sif0_queue_.clear();
    schedule_sif0();
}

void SifLink::schedule_sif0() {
    if (machine_ == nullptr) {
        return;
    }
    if (sif0_event_ != 0) {
        machine_->events().cancel(sif0_event_);
        sif0_event_ = 0;
    }
    if (!sif0_armed_ || sif0_queue_.empty()) {
        return;
    }
    const auto& [key, head] = *sif0_queue_.begin();
    std::uint64_t bytes = head.packet.size();
    for (const EeWrite& w : head.writes) {
        bytes += w.bytes.size();
    }
    const Cycles when = std::max(
        {key.first, sif0_armed_at_ + transfer_cost(machine_->cost(), bytes), machine_->clock().now()});
    sif0_event_ = machine_->events().schedule(when, "sif0_deliver", [this](Cycles at) { deliver_sif0(at); });
}

void SifLink::deliver_sif0(Cycles) {
    sif0_event_ = 0;
    if (!sif0_armed_ || sif0_queue_.empty()) {
        return;
    }
    auto node = sif0_queue_.extract(sif0_queue_.begin());
    Sif0Delivery& d = node.mapped();
    GuestMemory& mem = machine_->memory();
    for (const EeWrite& w : d.writes) {
        try {
            mem.write_bytes(w.address, w.bytes);
        } catch (const std::out_of_range&) {
            fail("IOP data for the EE at " + hex32(w.address) + " + " + std::to_string(w.bytes.size()) +
                 " is not RAM");
        }
    }
    if (!d.packet.empty()) {
        const auto receive = ee_receive_buffer();
        if (!receive) {
            fail("an IOP packet (" + std::string(sif::command_name(sif::load32(d.packet, sif::hdr_cid))) +
                 ") is due but the EE has not given a receive buffer");
        }
        try {
            mem.write_bytes(*receive, d.packet);
        } catch (const std::out_of_range&) {
            fail("the EE receive buffer " + hex32(*receive) + " is not RAM");
        }
        if (machine_->tracing(TraceCategory::sif)) {
            const std::uint32_t cid = sif::load32(d.packet, sif::hdr_cid);
            machine_->trace(TraceCategory::sif, "EE receives " + hex32(cid) + " (" +
                                                    std::string(sif::command_name(cid)) + ") after " +
                                                    std::to_string(d.writes.size()) + " data writes");
        }
        sif0_armed_ = false;
        machine_->interrupts().raise_dmac(DmaChannel::sif0);
    }
    ++delivered_;
    machine_->note_activity();
    schedule_sif0();
}

void SifLink::fail(std::string message) const {
    StopInfo info;
    info.reason = StopReason::guest_error;
    info.message = "iop: " + std::move(message);
    machine_->stop(std::move(info));
    std::abort(); // stop() never returns
}

std::string_view sif::command_name(std::uint32_t cid) noexcept {
    switch (cid) {
    case cid_change_receive:
        return "change receive buffer";
    case cid_set_sreg:
        return "set software register";
    case cid_init:
        return "init";
    case cid_reset:
        return "reset";
    case cid_rpc_end:
        return "rpc end";
    case cid_rpc_bind:
        return "rpc bind";
    case cid_rpc_call:
        return "rpc call";
    case cid_rpc_rdata:
        return "rpc rdata";
    case cid_fileio_done:
        return "fileio completion";
    default:
        return "unknown command";
    }
}

} // namespace dq8::rt
