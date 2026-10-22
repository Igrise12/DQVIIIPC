// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/iop_exec.h"

#include "runtime/iop/iop_memory.h"
#include "runtime/iop/iop_rpc.h"
#include "runtime/iop/iop_server.h"
#include "runtime/iop/kernel/hle.h"
#include "runtime/iop/loader/irx_image.h"
#include "runtime/iop/sif_link.h"
#include "runtime/iop/sif_protocol.h"
#include "runtime/machine/cost_model.h"
#include "runtime/machine/machine_services.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

// The thread a module's start function runs on: high priority (it returns before the threads it starts run
// unless they block it), a 16 KiB stack. Inferred; to confirm in bring-up (Protocol notes).
constexpr std::int32_t start_priority = 8;
constexpr std::uint32_t start_stack = 0x4000;
// The interrupt context's stack.
constexpr std::uint32_t irq_stack_size = 0x2000;
// Module start results.
constexpr std::int32_t no_resident_end = 1;
// IOP interrupt numbers of the DMA channels (intrman): 0x20 + channel for 0-6, 0x28 + (channel - 7) after.
constexpr std::uint32_t irq_dma_base = 0x20;
constexpr std::uint32_t irq_dma2_base = 0x28;
constexpr std::uint32_t sif_dma_history = 256;

std::uint32_t dma_irq(std::uint32_t channel) {
    return channel < 7 ? irq_dma_base + channel : irq_dma2_base + (channel - 7);
}

// An interpreted module's RPC server as IopRpcLayer sees it: every call is queued for the module's server
// thread and answered later.
class GuestRpcServer final : public IopRpcServer {
  public:
    GuestRpcServer(IopExec& exec, std::uint32_t sid, std::uint32_t data, std::string module)
        : exec_(exec), sid_(sid), data_(data), name_(module + ":" + hex32(sid)) {}

    std::uint32_t sid() const noexcept override { return sid_; }
    std::string_view name() const noexcept override { return name_; }
    std::uint32_t receive_buffer_size() const noexcept override { return 0; }
    RpcReply call(IopCallContext& call, std::span<const std::uint8_t> args) override {
        call.deferred = true;
        exec_.enqueue_request(IopGuestRequest{call.request, data_, call.fno,
                                              static_cast<std::uint32_t>(args.size()), call.recv_size});
        return {};
    }

  private:
    IopExec& exec_;
    std::uint32_t sid_;
    std::uint32_t data_;
    std::string name_;
};

} // namespace

IopExec::IopExec(MachineServices& machine, IopMemory& ram, SifLink& link, IopRpcLayer& rpc,
                 std::vector<IopDeviceMapping> devices, bool strict_imports)
    : machine_(machine), ram_(ram), link_(link), rpc_(rpc), strict_imports_(strict_imports),
      hw_(std::make_unique<IopHardware>(machine, ram, *this, std::move(devices))),
      cpu_(std::make_unique<IopCpu>(ram.data(), *hw_)) {
    for (const SlotKind k :
         {SlotKind::thread_exit, SlotKind::start_return, SlotKind::call_return, SlotKind::irq_return}) {
        slots_.push_back(Slot{k, nullptr, {}, 0});
    }
    std::map<std::string, std::map<std::uint16_t, std::uint32_t>> by_library;
    for (const HleFunction& f : hle_functions()) {
        by_library[std::string(f.library)][f.ordinal] = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(Slot{SlotKind::hle, &f, {}, 0});
    }
    for (auto& [name, ordinals] : by_library) {
        libraries_.add_hle(name, std::move(ordinals));
    }
    fixed_slots_ = slots_.size();
    hw_->set_hle_slots(static_cast<std::uint32_t>(slots_.size()));
}

IopExec::~IopExec() = default;

void IopExec::connect_devices() {
    hw_->connect_devices();
}

const CostModel& IopExec::cost() const noexcept {
    return machine_.cost();
}

std::uint64_t IopExec::instructions() const noexcept {
    return cpu_->instructions();
}

std::uint64_t IopExec::sysclock() const noexcept {
    return (now_ - boot_) / ee_cycles_per_iop_cycle;
}

void IopExec::reset() {
    EventQueue& ev = machine_.events();
    if (slice_event_ != 0) {
        ev.cancel(slice_event_);
    }
    for (auto& [id, t] : threads_) {
        if (t.delay_event != 0) {
            ev.cancel(t.delay_event);
        }
    }
    for (auto& [id, a] : alarms_) {
        if (a.event != 0) {
            ev.cancel(a.event);
        }
    }
    for (IopHardTimer& t : timers_) {
        if (t.event != 0) {
            ev.cancel(t.event);
        }
    }
    slice_event_ = 0;
    hw_->reset();
    slots_.resize(fixed_slots_);
    hw_->set_hle_slots(static_cast<std::uint32_t>(slots_.size()));
    libraries_.clear_guest();
    now_ = machine_.clock().now();
    boot_ = now_;
    limit_ = 0;
    run_limit_ = 0;
    in_slice_ = false;
    poll_count_ = 0;
    modules_.clear();
    starts_.clear();
    threads_.clear();
    for (auto& q : ready_) {
        q.clear();
    }
    current_ = 0;
    next_thread_id_ = 1;
    intr_handlers_.clear();
    irq_jobs_.clear();
    in_irq_ = false;
    irq_stack_ = 0;
    semas_.clear();
    event_flags_.clear();
    next_object_id_ = 1;
    alarms_.clear();
    next_alarm_id_ = 1;
    timers_ = {};
    guest_servers_.clear();
    rpc_queues_.clear();
    clients_.clear();
    next_rpc_id_ = 0;
    sif_dma_done_.clear();
    next_sif_dma_ = 1;
    print_line_.clear();
}

// ---- Modules
// -----------------------------------------------------------------------------------------------

void IopExec::ensure_irq_stack() {
    if (irq_stack_ != 0) {
        return;
    }
    const std::uint32_t block = alloc(1, irq_stack_size, 0);
    if (block == 0) {
        stop(StopReason::guest_error, "IOP RAM has no room for the interrupt stack", 0);
    }
    irq_stack_ = block + irq_stack_size;
}

void IopExec::load_module(std::string name, std::int32_t id, std::span<const std::uint8_t> file,
                          std::string_view path, std::span<const std::uint8_t> args, StartDone done) {
    IrxModule irx;
    try {
        irx = parse_irx(file);
    } catch (const IrxError& e) {
        stop(StopReason::unsupported, "loading " + name + " (" + std::string(path) + "): " + e.what(), 0);
    }
    ensure_irq_stack();
    const std::uint32_t base = alloc(0, irx.mem_size, 0);
    if (base == 0) {
        stop(StopReason::guest_error,
             "IOP RAM has no room for " + name + " (" + std::to_string(irx.mem_size) + " bytes)", 0);
    }
    std::vector<std::uint8_t> image;
    try {
        image = relocate_irx(irx, base);
    } catch (const IrxError& e) {
        stop(StopReason::unsupported, "loading " + name + " (" + std::string(path) + "): " + e.what(), 0);
    }
    ram_.write(base, image);

    Module m;
    m.name = name;
    m.id = id;
    m.base = base;
    m.size = irx.mem_size;
    m.entry = base + irx.entry;
    m.gp = base + irx.gp;
    m.text_size = irx.text_size;
    modules_.push_back(m);
    const std::size_t index = modules_.size() - 1;
    link_module(modules_[index]);
    machine_.trace(TraceCategory::iop, "load " + name + " at " + hex32(base) + " (" + std::to_string(m.size) +
                                           " bytes, entry " + hex32(m.entry) + ", gp " + hex32(m.gp) + ")");

    // The start function runs on a thread of its own: start(argc, argv) with argv[0] the path and the
    // arguments after it, the strings and the vector at the top of its stack.
    const std::int32_t tid =
        create_thread(0, 0, modules_[index].entry, start_stack, start_priority, modules_[index].gp, name);
    if (tid < 0) {
        stop(StopReason::guest_error, "IOP RAM has no room for the start thread of " + name, 0);
    }
    IopThread& t = threads_.at(tid);
    t.start_thread = true;
    std::vector<std::string> argv{std::string(path)};
    std::string current;
    for (const std::uint8_t c : args) {
        if (c == 0) {
            argv.push_back(current);
            current.clear();
        } else {
            current.push_back(static_cast<char>(c));
        }
    }
    if (!current.empty()) {
        argv.push_back(current);
    }
    std::uint32_t cursor = t.stack + t.stack_size - 16;
    std::vector<std::uint32_t> pointers;
    for (const std::string& a : argv) {
        cursor -= static_cast<std::uint32_t>(a.size() + 1);
        std::vector<std::uint8_t> bytes(a.begin(), a.end());
        bytes.push_back(0);
        ram_.write(cursor, bytes);
        pointers.push_back(cursor);
    }
    cursor &= ~3u;
    cursor -= static_cast<std::uint32_t>(4 * (pointers.size() + 1));
    for (std::size_t i = 0; i <= pointers.size(); ++i) {
        ram_.write32(cursor + static_cast<std::uint32_t>(4 * i), i < pointers.size() ? pointers[i] : 0);
    }
    start_thread(t, 0);
    t.cpu.gpr[4] = static_cast<std::uint32_t>(pointers.size());
    t.cpu.gpr[5] = cursor;
    t.cpu.gpr[29] = (cursor - 16) & ~7u;
    t.cpu.gpr[31] = slot_address(static_cast<std::uint32_t>(SlotKind::start_return));
    t.resume = IopResume::module_start;
    t.resume_arg = static_cast<std::uint32_t>(index);
    starts_.push_back(PendingStart{index, tid, std::move(done)});
    wake_iop(machine_.clock().now());
}

std::uint32_t IopExec::unimplemented_slot(const std::string& library, std::uint16_t ordinal) {
    for (std::size_t i = fixed_slots_; i < slots_.size(); ++i) {
        if (slots_[i].library == library && slots_[i].ordinal == ordinal) {
            return static_cast<std::uint32_t>(i);
        }
    }
    if (slots_.size() >= iop_map::hle_slot_count) {
        stop(StopReason::unsupported, "the HLE page is full", 0);
    }
    slots_.push_back(Slot{SlotKind::unimplemented, nullptr, library, ordinal});
    hw_->set_hle_slots(static_cast<std::uint32_t>(slots_.size()));
    return static_cast<std::uint32_t>(slots_.size() - 1);
}

void IopExec::link_module(Module& m) {
    std::vector<IrxImportTable> tables;
    try {
        tables = find_irx_imports(ram_, m.base, m.text_size);
    } catch (const IrxError& e) {
        stop(StopReason::unsupported, "linking " + m.name + ": " + e.what(), 0);
    }
    std::vector<std::string> unimplemented;
    for (const IrxImportTable& table : tables) {
        for (const IrxImportStub& stub : table.stubs) {
            const IopLinkTarget target = libraries_.resolve(table.library, table.version, stub.ordinal);
            std::uint32_t address = 0;
            switch (target.kind) {
            case IopLinkTarget::Kind::hle:
                address = slot_address(target.slot);
                ++m.hle_imports;
                break;
            case IopLinkTarget::Kind::guest:
                address = target.address;
                ++m.guest_imports;
                break;
            case IopLinkTarget::Kind::unimplemented:
                address = slot_address(unimplemented_slot(table.library, stub.ordinal));
                ++m.unimplemented_imports;
                unimplemented.push_back(table.library + " " + std::to_string(stub.ordinal));
                machine_.emit(MachineEvent{"iop_import_unimplemented", {}}
                                  .add("module", m.name)
                                  .add("library", table.library)
                                  .add("ordinal", std::uint64_t{stub.ordinal}));
                break;
            case IopLinkTarget::Kind::missing_library:
                stop(StopReason::unsupported,
                     m.name + " imports library " + table.library + " (version " + hex32(table.version) +
                         "), which no HLE library or loaded module provides",
                     stub.address);
            }
            if ((address & 0xF0000000u) != ((stub.address + 4) & 0xF0000000u)) {
                stop(StopReason::unsupported,
                     m.name + ": the import stub at " + hex32(stub.address) + " cannot jump to " +
                         hex32(address),
                     stub.address);
            }
            ram_.write32(stub.address, 0x08000000u | ((address >> 2) & 0x03FFFFFFu)); // j target
        }
    }
    machine_.emit(MachineEvent{"iop_link", {}}
                      .add("module", m.name)
                      .add("hle", std::uint64_t{m.hle_imports})
                      .add("guest", std::uint64_t{m.guest_imports})
                      .add("unimplemented", std::uint64_t{m.unimplemented_imports}));
    if (strict_imports_ && !unimplemented.empty()) {
        std::string list;
        for (const std::string& u : unimplemented) {
            list += (list.empty() ? "" : ", ") + u;
        }
        stop(StopReason::unsupported, m.name + " imports HLE functions nobody implemented: " + list, m.base);
    }
}

const IopExec::Module* IopExec::module_containing(std::uint32_t address) const {
    const std::uint32_t phys = iop_physical(address);
    if (phys >= iop_ram_mirror_end) {
        return nullptr;
    }
    const std::uint32_t offset = phys & (iop_ram_bytes - 1);
    for (const Module& m : modules_) {
        if (offset >= m.base && offset - m.base < m.size) {
            return &m;
        }
    }
    return nullptr;
}

std::string IopExec::module_at(std::uint32_t pc) const {
    const Module* m = module_containing(pc);
    return m != nullptr ? m->name : std::string("?");
}

std::string IopExec::location(std::uint32_t pc) const {
    if (const Module* m = module_containing(pc)) {
        return m->name + "+" + hex32((iop_physical(pc) & (iop_ram_bytes - 1)) - m->base);
    }
    if (const auto slot = hw_->hle_slot(iop_physical(pc))) {
        const Slot& s = slots_[*slot];
        if (s.function != nullptr) {
            return "HLE " + std::string(s.function->library) + "::" + std::string(s.function->name);
        }
        return "HLE slot " + std::to_string(*slot);
    }
    return hex32(pc);
}

std::string IopExec::context_name(const IopThread* t) const {
    if (t == nullptr) {
        return "the IOP interrupt context (handler " + location(irq_job_.function) + ")";
    }
    return "IOP thread " + std::to_string(t->id) + " (" + t->module + ")";
}

void IopExec::stop(StopReason reason, const std::string& message, std::uint32_t pc, std::uint32_t address) {
    StopInfo info;
    info.reason = reason;
    info.message = "iop: " + message;
    info.pc = pc;
    info.address = address;
    machine_.stop(std::move(info));
    std::abort(); // stop() never returns
}

void IopExec::finish_module_start(IopThread& t) {
    const auto index = static_cast<std::size_t>(t.resume_arg);
    const auto result = static_cast<std::int32_t>(t.cpu.gpr[2]);
    const std::int32_t tid = t.id;
    Module& m = modules_.at(index);
    m.started = true;
    m.start_result = result;
    if (result == no_resident_end) {
        m.resident = false;
        free(m.base);
    }
    StartDone done;
    for (auto it = starts_.begin(); it != starts_.end(); ++it) {
        if (it->thread == tid) {
            done = std::move(it->done);
            starts_.erase(it);
            break;
        }
    }
    if (current_ == tid) {
        current_ = 0;
    }
    erase_thread(tid);
    machine_.trace(TraceCategory::iop, m.name + " start returned " + std::to_string(result));
    if (done) {
        done(m, result);
    }
}

// ---- Scheduling
// ----------------------------------------------------------------------------------------------

void IopExec::wake_iop(Cycles at) {
    if (in_slice_ || slice_event_ != 0) {
        return; // the running or scheduled slice looks again
    }
    now_ = std::max(now_, at);
    if (runnable()) {
        schedule_slice(now_);
    }
}

void IopExec::schedule_slice(Cycles at) {
    slice_event_ = machine_.events().schedule(at, "iop_slice", [this](Cycles when) { run_slice(when); });
}

bool IopExec::irq_ready() const {
    if (!in_irq_ && irq_jobs_.empty() && hw_->pending_lines() == 0) {
        return false;
    }
    const auto it = threads_.find(current_);
    return it == threads_.end() || it->second.interrupts;
}

bool IopExec::runnable() const {
    if (in_irq_ || irq_ready()) {
        return true;
    }
    if (threads_.count(current_) != 0) {
        return true;
    }
    return std::any_of(ready_.begin(), ready_.end(), [](const auto& q) { return !q.empty(); });
}

void IopExec::refresh_limit() {
    if (const auto next = machine_.events().next_time(); next && *next < limit_) {
        limit_ = *next;
    }
    run_limit_ = std::min(run_limit_, limit_);
}

void IopExec::run_slice(Cycles when) {
    slice_event_ = 0;
    now_ = std::max(now_, when);
    ++slices_;
    in_slice_ = true;
    struct SliceGuard {
        bool& flag;
        ~SliceGuard() { flag = false; }
    } guard{in_slice_};
    limit_ = now_ + cost().iop_slice;
    run_limit_ = limit_;
    refresh_limit();
    while (now_ < limit_) {
        if (!dispatch()) {
            break;
        }
    }
    in_slice_ = false;
    if (runnable()) {
        schedule_slice(now_);
    }
}

bool IopExec::dispatch() {
    if (in_irq_) {
        run_context(irq_cpu_, nullptr);
        return true;
    }
    if (irq_ready()) {
        collect_interrupts();
        if (!irq_jobs_.empty()) {
            begin_irq_job();
            run_context(irq_cpu_, nullptr);
            return true;
        }
    }
    IopThread* t = select_thread();
    if (t == nullptr) {
        return false;
    }
    run_context(t->cpu, t);
    return true;
}

IopThread* IopExec::select_thread() {
    IopThread* best = nullptr;
    for (auto& q : ready_) {
        if (!q.empty()) {
            best = &threads_.at(q.front());
            break;
        }
    }
    if (current_ != 0) {
        IopThread& cur = threads_.at(current_);
        if (best == nullptr || best->priority >= cur.priority) {
            return &cur;
        }
        // Preempted: it stays first in its queue.
        current_ = 0;
        make_ready(cur, true);
    }
    if (best == nullptr) {
        return nullptr;
    }
    unready(*best);
    best->status = IopThreadStatus::running;
    current_ = best->id;
    return best;
}

void IopExec::make_ready(IopThread& t, bool front) {
    t.status = IopThreadStatus::ready;
    t.wait = IopWait::none;
    auto& q = ready_[static_cast<std::size_t>(t.priority)];
    if (front) {
        q.push_front(t.id);
    } else {
        q.push_back(t.id);
    }
}

void IopExec::unready(IopThread& t) {
    auto& q = ready_[static_cast<std::size_t>(t.priority)];
    q.erase(std::remove(q.begin(), q.end(), t.id), q.end());
}

void IopExec::run_context(IopCpuState& cpu, IopThread* t) {
    run_limit_ = limit_;
    IopCpuStop stop_info;
    const IopCpuExit exit = cpu_->run(cpu, now_, run_limit_, cost().iop_instruction, stop_info);
    if (exit == IopCpuExit::trap) {
        handle_trap(cpu, t, stop_info.trap);
        refresh_limit(); // what the trap did may have scheduled an earlier event (an RPC reply, a wake)
    } else if (exit == IopCpuExit::fault) {
        fault(cpu, t, stop_info);
    }
}

void IopExec::handle_trap(IopCpuState& cpu, IopThread* t, std::uint32_t slot) {
    if (slot >= slots_.size()) {
        stop(StopReason::guest_error, "a jump to HLE slot " + std::to_string(slot) + ", which is not in use",
             cpu.pc);
    }
    const SlotKind kind = slots_[slot].kind;
    switch (kind) {
    case SlotKind::hle: {
        const HleFunction& f = *slots_[slot].function;
        cpu.pc = cpu.gpr[31];
        cpu.branch_pending = false;
        now_ += cost().iop_hle_call;
        ++hle_calls_;
        poll_count_ = 0;
        HleCall call(*this, cpu, t, f);
        f.fn(call);
        return;
    }
    case SlotKind::unimplemented: {
        const Slot& s = slots_[slot];
        const std::uint32_t caller = cpu.gpr[31] - 8;
        stop(StopReason::unsupported,
             module_at(caller) + " called " + s.library + " ordinal " + std::to_string(s.ordinal) +
                 ", which has no HLE implementation (" + context_name(t) + ", call at " + location(caller) +
                 ")",
             caller);
    }
    case SlotKind::thread_exit:
        if (t == nullptr) {
            stop(StopReason::guest_error, "an interrupt handler returned through the thread exit", cpu.pc);
        }
        thread_exit(*t);
        return;
    case SlotKind::start_return:
        if (t == nullptr || t->resume != IopResume::module_start) {
            stop(StopReason::guest_error, "a return to the module start trap outside a module start", cpu.pc);
        }
        finish_module_start(*t);
        return;
    case SlotKind::call_return:
        if (t == nullptr || t->resume != IopResume::rpc_loop || t->request == 0) {
            stop(StopReason::guest_error, "a return to the RPC trap outside an RPC function", cpu.pc);
        }
        complete_request(*t);
        return;
    case SlotKind::irq_return:
        if (t != nullptr || !in_irq_) {
            stop(StopReason::guest_error, context_name(t) + " returned through the interrupt return", cpu.pc);
        }
        finish_irq_job();
        return;
    }
}

void IopExec::fault(const IopCpuState& cpu, IopThread* t, const IopCpuStop& f) {
    (void)cpu;
    const bool data = f.fault == IopFault::load_bus || f.fault == IopFault::store_bus ||
                      f.fault == IopFault::load_address || f.fault == IopFault::store_address;
    std::string message = std::string(to_string(f.fault));
    if (f.instruction != 0) {
        message += " (instruction " + hex32(f.instruction) + ")";
    }
    message += " in " + context_name(t) + " at pc " + hex32(f.pc) + " (" + location(f.pc) + ")";
    if (f.delay_slot) {
        message += ", in a branch delay slot";
    }
    if (data) {
        message += ", address " + hex32(f.address);
        if (f.fault == IopFault::load_bus || f.fault == IopFault::store_bus) {
            message += " (" + hw_->describe(iop_physical(f.address)) + ")";
        }
    }
    StopInfo info;
    const bool mmio = (f.fault == IopFault::load_bus || f.fault == IopFault::store_bus) &&
                      iop_physical(f.address) >= iop_map::scratchpad;
    info.reason = mmio ? StopReason::unsupported : StopReason::guest_exception;
    info.message = "iop: " + message;
    info.pc = f.pc;
    info.address = data ? f.address : f.pc;
    info.has_number = true;
    info.number = iop_exception_code(f.fault);
    machine_.stop(std::move(info));
    std::abort(); // stop() never returns
}

void IopExec::thread_exit(IopThread& t) {
    if (t.resume == IopResume::module_start) {
        stop(StopReason::guest_error, "a module start thread reached the thread exit", t.cpu.pc);
    }
    t.status = IopThreadStatus::dormant;
    t.wait = IopWait::none;
    if (current_ == t.id) {
        current_ = 0;
    }
    machine_.trace(TraceCategory::iop, "thread " + std::to_string(t.id) + " (" + t.module + ") returned");
}

// ---- Interrupts
// --------------------------------------------------------------------------------------------

void IopExec::queue_irq(const IopIrqJob& job, Cycles at) {
    irq_jobs_.push_back(job);
    wake_iop(at);
}

void IopExec::collect_interrupts() {
    const std::uint32_t pending = hw_->pending_lines();
    for (std::uint32_t line = 0; line < 32; ++line) {
        if ((pending & (1u << line)) == 0) {
            continue;
        }
        hw_->acknowledge_line(line);
        if (line == static_cast<std::uint32_t>(IopIrq::dma)) {
            const std::uint32_t flags = hw_->dma_flags();
            for (std::uint32_t ch = 0; ch < 13; ++ch) {
                if ((flags & (1u << ch)) == 0) {
                    continue;
                }
                hw_->acknowledge_dma(ch);
                const std::uint32_t irq = dma_irq(ch);
                const auto h = intr_handlers_.find(irq);
                if (h == intr_handlers_.end()) {
                    stop(StopReason::unsupported,
                         "IOP DMA channel " + std::to_string(ch) + " finished with its interrupt (" +
                             hex32(irq) + ") enabled and no handler registered",
                         0);
                }
                irq_jobs_.push_back(IopIrqJob{IopIrqJob::Kind::intr_handler, h->second.function,
                                              h->second.arg, h->second.gp, irq});
            }
            continue;
        }
        const auto h = intr_handlers_.find(line);
        if (h == intr_handlers_.end()) {
            stop(StopReason::unsupported,
                 "IOP interrupt line " + std::to_string(line) + " was raised while enabled, with no handler",
                 0);
        }
        irq_jobs_.push_back(
            IopIrqJob{IopIrqJob::Kind::intr_handler, h->second.function, h->second.arg, h->second.gp, line});
    }
}

void IopExec::begin_irq_job() {
    irq_job_ = irq_jobs_.front();
    irq_jobs_.pop_front();
    irq_cpu_ = IopCpuState{};
    irq_cpu_.gpr[4] = irq_job_.arg;
    irq_cpu_.gpr[28] = irq_job_.gp;
    irq_cpu_.gpr[29] = irq_stack_ - 16;
    irq_cpu_.gpr[31] = slot_address(static_cast<std::uint32_t>(SlotKind::irq_return));
    irq_cpu_.pc = irq_job_.function;
    in_irq_ = true;
}

void IopExec::finish_irq_job() {
    in_irq_ = false;
    const std::uint32_t result = irq_cpu_.gpr[2];
    switch (irq_job_.kind) {
    case IopIrqJob::Kind::intr_handler:
        // A handler that returns 0 has its interrupt disabled (intrman; inferred, Protocol notes).
        if (result == 0) {
            const std::uint32_t irq = irq_job_.ref;
            if (irq < 32) {
                hw_->set_i_mask(hw_->i_mask() & ~(1u << irq));
            } else {
                const std::uint32_t ch =
                    irq >= irq_dma2_base ? 7 + (irq - irq_dma2_base) : irq - irq_dma_base;
                hw_->set_dma_interrupt(ch, false);
            }
            machine_.trace(TraceCategory::iop, "interrupt " + hex32(irq) + " disabled by its handler");
        }
        break;
    case IopIrqJob::Kind::timer: {
        IopHardTimer& t = timers_.at(irq_job_.ref);
        if (!t.running) {
            break;
        }
        if (result == 0) {
            t.running = false;
            break;
        }
        t.compare = result;
        schedule_timer(irq_job_.ref, t.started);
        break;
    }
    case IopIrqJob::Kind::alarm: {
        const auto it = alarms_.find(irq_job_.ref);
        if (it == alarms_.end()) {
            break; // cancelled from its own handler
        }
        if (result == 0) {
            alarms_.erase(it);
            break;
        }
        IopAlarm& a = it->second;
        a.running = false;
        a.due += cycles_of_ticks(result);
        const std::uint32_t id = a.id;
        a.event = machine_.events().schedule(std::max(a.due, now_), "iop_alarm",
                                             [this, id](Cycles when) { alarm_due(id, when); });
        break;
    }
    case IopIrqJob::Kind::rpc_end:
        break;
    }
    refresh_limit();
}

// ---- Hardware owner
// ----------------------------------------------------------------------------------------

void IopExec::hw_interrupt(Cycles at) {
    if (in_slice_) {
        run_limit_ = now_; // leave the CPU run so the dispatcher sees it
        return;
    }
    wake_iop(at);
}

void IopExec::hw_poll(std::uint32_t pc, std::uint32_t address, std::uint32_t value) {
    if (pc == poll_pc_ && address == poll_address_ && value == poll_value_) {
        if (++poll_count_ < cost().iop_spin_reads) {
            return;
        }
        // The value cannot change before something happens: jump to the next event.
        poll_count_ = 0;
        if (const auto next = machine_.events().next_time(); next && *next > now_) {
            now_ = std::min(*next, now_ + cost().spin_quantum);
        }
        run_limit_ = now_;
        return;
    }
    poll_pc_ = pc;
    poll_address_ = address;
    poll_value_ = value;
    poll_count_ = 1;
}

void IopExec::hw_activity() {
    poll_count_ = 0;
    if (in_slice_) {
        refresh_limit();
    }
}

void IopExec::hw_stop(std::string message, std::uint32_t pc, std::uint32_t address) {
    stop(StopReason::unsupported, message + " (pc " + hex32(pc) + ", " + location(pc) + ")", pc, address);
}

// ---- SIF RPC
// -----------------------------------------------------------------------------------------------

std::int32_t IopExec::register_server(const IopGuestServer& server) {
    if (guest_servers_.count(server.data) != 0) {
        stop(StopReason::guest_error,
             server.module + " registers the server structure at " + hex32(server.data) + " twice", 0);
    }
    try {
        rpc_.add_guest(std::make_unique<GuestRpcServer>(*this, server.sid, server.data, server.module),
                       server.module, server.data, server.buffer, server.cbuffer);
    } catch (const std::invalid_argument&) {
        stop(StopReason::unsupported,
             server.module + " registers RPC server " + hex32(server.sid) +
                 ", which another module or a native service already serves",
             0);
    }
    guest_servers_[server.data] = server;
    machine_.emit(MachineEvent{"iop_rpc_register", {}}
                      .add("sid", std::uint64_t{server.sid})
                      .add("module", server.module));
    return 0;
}

void IopExec::remove_server(std::uint32_t data) {
    const auto it = guest_servers_.find(data);
    if (it == guest_servers_.end()) {
        return;
    }
    rpc_.remove(it->second.sid);
    guest_servers_.erase(it);
}

void IopExec::enqueue_request(const IopGuestRequest& request) {
    const auto server = guest_servers_.find(request.server);
    if (server == guest_servers_.end()) {
        stop(StopReason::guest_error,
             "an RPC call for the unregistered server structure " + hex32(request.server), 0);
    }
    const auto q = rpc_queues_.find(server->second.queue);
    if (q == rpc_queues_.end()) {
        stop(StopReason::guest_error,
             "RPC server " + hex32(server->second.sid) + " was called before its queue was set up", 0);
    }
    q->second.pending.push_back(request);
    if (q->second.thread != 0) {
        IopThread* t = thread(q->second.thread);
        if (t != nullptr && t->status == IopThreadStatus::waiting && t->wait == IopWait::rpc_server &&
            t->wait_id == q->second.address) {
            serve_next(*t, q->second);
        }
    }
    wake_iop(machine_.clock().now());
}

void IopExec::rpc_loop(IopThread& t, std::uint32_t queue) {
    const auto q = rpc_queues_.find(queue);
    if (q == rpc_queues_.end()) {
        stop(StopReason::guest_error, "sceSifRpcLoop on the queue " + hex32(queue) + ", which is not set up",
             t.cpu.gpr[31]);
    }
    q->second.thread = t.id;
    t.loop_sp = t.cpu.gpr[29];
    t.resume = IopResume::rpc_loop;
    t.resume_arg = queue;
    serve_next(t, q->second);
}

void IopExec::serve_next(IopThread& t, IopRpcQueue& q) {
    while (!q.pending.empty()) {
        const IopGuestRequest req = q.pending.front();
        q.pending.pop_front();
        const auto server = guest_servers_.find(req.server);
        if (server == guest_servers_.end()) {
            rpc_.complete(req.request, RpcReply{}); // removed while queued: an empty answer
            continue;
        }
        const IopGuestServer& s = server->second;
        // The fields a server function may read in its server structure.
        ram_.write32(iop_physical(s.data + 0x0C) & (iop_ram_bytes - 1), req.send_size);
        ram_.write32(iop_physical(s.data + 0x24) & (iop_ram_bytes - 1), req.fno);
        ram_.write32(iop_physical(s.data + 0x2C) & (iop_ram_bytes - 1), req.recv_size);
        t.request = req.request;
        t.request_recv = req.recv_size;
        t.resume = IopResume::rpc_loop;
        t.resume_arg = q.address;
        t.cpu.retire_load();
        t.cpu.branch_pending = false;
        t.cpu.gpr[4] = req.fno;
        t.cpu.gpr[5] = s.buffer;
        t.cpu.gpr[6] = req.send_size;
        t.cpu.gpr[28] = s.gp;
        t.cpu.gpr[29] = t.loop_sp;
        t.cpu.gpr[31] = slot_address(static_cast<std::uint32_t>(SlotKind::call_return));
        t.cpu.pc = s.function;
        if (t.status == IopThreadStatus::waiting) {
            if (t.delay_event != 0) {
                machine_.events().cancel(t.delay_event);
                t.delay_event = 0;
            }
            make_ready(t);
        }
        return;
    }
    if (t.status == IopThreadStatus::running || t.status == IopThreadStatus::ready) {
        if (t.status == IopThreadStatus::ready) {
            unready(t);
        }
        block(t, IopWait::rpc_server, q.address);
    }
}

void IopExec::complete_request(IopThread& t) {
    const std::uint32_t rdata = t.cpu.gpr[2];
    std::vector<std::uint8_t> data;
    if (t.request_recv != 0) {
        const std::uint32_t phys = iop_physical(rdata);
        const std::uint32_t at = phys & (iop_ram_bytes - 1);
        if (phys >= iop_ram_mirror_end || t.request_recv > iop_ram_bytes - at) {
            stop(StopReason::guest_error,
                 "an RPC function returned the reply pointer " + hex32(rdata) + " for " +
                     std::to_string(t.request_recv) + " bytes (" + context_name(&t) + ")",
                 t.cpu.pc);
        }
        data.resize(t.request_recv);
        ram_.read(at, data);
    }
    const std::uint64_t request = t.request;
    t.request = 0;
    rpc_.complete(request, RpcReply{std::move(data), {}, now_ + cost().rpc_reply(t.request_recv)});
    const auto q = rpc_queues_.find(t.resume_arg);
    if (q == rpc_queues_.end()) {
        stop(StopReason::guest_error,
             "the RPC queue " + hex32(t.resume_arg) + " went away while it was served", t.cpu.pc);
    }
    serve_next(t, q->second);
}

void IopExec::client_bind(HleCall& call, std::uint32_t client, std::uint32_t sid, std::uint32_t mode) {
    if (clients_.count(client) != 0) {
        call.fail("the client structure " + hex32(client) + " already has a request in flight");
    }
    const std::uint32_t id = ++next_rpc_id_;
    IopThread* t = call.thread();
    call.write32(client + 0x00, client);
    call.write32(client + 0x04, id);
    call.write32(client + 0x08, t != nullptr ? static_cast<std::uint32_t>(t->id) : 0);
    call.write32(client + 0x0C, mode);
    call.write32(client + 0x10, sif::cid_rpc_bind);
    call.write32(client + 0x14, 0);
    call.write32(client + 0x24, 0);
    std::vector<std::uint8_t> p(sif::bind_size, 0);
    sif::store32(p, sif::hdr_sizes, sif::bind_size);
    sif::store32(p, sif::hdr_cid, sif::cid_rpc_bind);
    sif::store32(p, sif::rpc_pkt_addr, client);
    sif::store32(p, sif::rpc_id, id);
    sif::store32(p, sif::rpc_client, client);
    sif::store32(p, sif::bind_sid, sid);
    link_.send_to_ee(std::move(p), {}, now_ + cost().rpc_reply(0));
    IopClientOp op;
    op.bind = true;
    op.rpc_id = id;
    op.mode = mode;
    const bool wait = (mode & 1u) == 0;
    if (wait && t == nullptr) {
        call.ret(iop_error::illegal_context);
        return;
    }
    if (wait) {
        op.thread = t->id;
    }
    clients_[client] = op;
    call.ret(0);
    if (wait) {
        block(*t, IopWait::rpc_client, client);
    }
    machine_.trace(TraceCategory::iop, module_at(call.cpu().gpr[31]) + " binds EE server " + hex32(sid));
}

void IopExec::client_call(HleCall& call, std::uint32_t client, std::uint32_t fno, std::uint32_t mode,
                          std::uint32_t send, std::uint32_t send_size, std::uint32_t recv,
                          std::uint32_t recv_size, std::uint32_t end_function, std::uint32_t end_param) {
    if (clients_.count(client) != 0) {
        call.fail("the client structure " + hex32(client) + " already has a request in flight");
    }
    const std::uint32_t server = call.read32(client + 0x24);
    const std::uint32_t buffer = call.read32(client + 0x14);
    if (server == 0) {
        call.fail("a call through the client structure " + hex32(client) + ", which is not bound");
    }
    const std::uint32_t id = ++next_rpc_id_;
    IopThread* t = call.thread();
    call.write32(client + 0x00, client);
    call.write32(client + 0x04, id);
    call.write32(client + 0x08, t != nullptr ? static_cast<std::uint32_t>(t->id) : 0);
    call.write32(client + 0x0C, mode);
    call.write32(client + 0x10, sif::cid_rpc_call);
    call.write32(client + 0x1C, end_function);
    call.write32(client + 0x20, end_param);
    std::vector<EeWrite> writes;
    if (send_size != 0) {
        writes.push_back(EeWrite{buffer & 0x1FFFFFFFu, call.read_bytes(send, send_size)});
    }
    std::vector<std::uint8_t> p(sif::call_size, 0);
    sif::store32(p, sif::hdr_sizes, sif::call_size | (send_size << 8));
    sif::store32(p, sif::hdr_extra_dest, send_size != 0 ? buffer : 0);
    sif::store32(p, sif::hdr_cid, sif::cid_rpc_call);
    sif::store32(p, sif::rpc_pkt_addr, client);
    sif::store32(p, sif::rpc_id, id);
    sif::store32(p, sif::rpc_client, client);
    sif::store32(p, sif::call_fno, fno);
    sif::store32(p, sif::call_send_size, send_size);
    sif::store32(p, sif::call_recv_addr, recv);
    sif::store32(p, sif::call_recv_size, recv_size);
    sif::store32(p, sif::call_rmode, 1); // always an end packet: it completes the request here
    sif::store32(p, sif::call_server, server);
    link_.send_to_ee(std::move(p), std::move(writes), now_ + cost().rpc_reply(send_size));
    IopClientOp op;
    op.rpc_id = id;
    op.mode = mode;
    op.end_function = end_function;
    op.end_param = end_param;
    op.gp = call.caller_gp();
    const bool wait = (mode & 1u) == 0;
    if (wait && t == nullptr) {
        call.ret(iop_error::illegal_context);
        return;
    }
    if (wait) {
        op.thread = t->id;
    }
    clients_[client] = op;
    call.ret(0);
    if (wait) {
        block(*t, IopWait::rpc_client, client);
    }
}

void IopExec::on_rpc_end(std::span<const std::uint8_t> packet) {
    // Matched by the client structure alone, as the IOP's own library does: the EE's bind reply does not copy
    // the request's rpc_id (its packet slot keeps whatever it held), so the id cannot tell a stale reply
    // apart. The command the packet ends must match the request's.
    const std::uint32_t client = sif::load32(packet, sif::rpc_client);
    const auto it = clients_.find(client);
    if (it == clients_.end()) {
        stop(StopReason::guest_error,
             "an RPC end packet for the IOP client structure " + hex32(client) +
                 ", which has no request in flight",
             0, client);
    }
    const std::uint32_t ended = sif::load32(packet, sif::end_cid);
    const std::uint32_t expected = it->second.bind ? sif::cid_rpc_bind : sif::cid_rpc_call;
    if (ended != expected) {
        stop(StopReason::guest_error,
             "an RPC end packet for the IOP client structure " + hex32(client) + " ends " + hex32(ended) +
                 " (" + std::string(sif::command_name(ended)) + "), but its request is " + hex32(expected) +
                 " (" + std::string(sif::command_name(expected)) + ")",
             0, client);
    }
    const IopClientOp op = it->second;
    clients_.erase(it);
    const std::uint32_t at = client & (iop_ram_bytes - 1);
    if (op.bind) {
        ram_.write32(at + 0x24, sif::load32(packet, sif::end_server));
        ram_.write32(at + 0x14, sif::load32(packet, sif::end_buffer));
        ram_.write32(at + 0x18, sif::load32(packet, sif::end_cbuffer));
    }
    const Cycles now = machine_.clock().now();
    if (op.end_function != 0) {
        irq_jobs_.push_back(
            IopIrqJob{IopIrqJob::Kind::rpc_end, op.end_function, op.end_param, op.gp, client});
    }
    if (op.thread != 0) {
        IopThread* t = thread(op.thread);
        if (t != nullptr && t->status == IopThreadStatus::waiting && t->wait == IopWait::rpc_client &&
            t->wait_id == client) {
            wake(*t, 0);
        }
    }
    wake_iop(now);
}

std::uint32_t IopExec::sif_dma(HleCall& call, std::uint32_t descriptors, std::uint32_t count) {
    if (count == 0 || count > sif::dma_max_descriptors) {
        return 0;
    }
    std::vector<EeWrite> writes;
    std::uint64_t bytes = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t d = descriptors + 16 * i;
        const std::uint32_t src = call.read32(d + 0x0);
        const std::uint32_t dest = call.read32(d + 0x4);
        const std::uint32_t size = call.read32(d + 0x8);
        const std::uint32_t attr = call.read32(d + 0xC);
        if ((attr & ~sif::dma_attr_end) != 0) {
            call.fail("transfer attribute " + hex32(attr) +
                      " (interrupts from IOP transfers are not modelled)");
        }
        const std::uint32_t words = (size + 3u) & ~3u;
        writes.push_back(EeWrite{dest & 0x1FFFFFFFu, call.read_bytes(src, words)});
        bytes += words;
    }
    std::uint32_t id = next_sif_dma_++;
    if (next_sif_dma_ == 0) {
        next_sif_dma_ = 1;
    }
    sif_dma_done_[id] = false;
    while (sif_dma_done_.size() > sif_dma_history) {
        sif_dma_done_.erase(sif_dma_done_.begin());
    }
    const Cycles ready = now_ + std::max<std::uint64_t>(1, (bytes + 15) / 16) * cost().dma_per_qword;
    link_.send_to_ee({}, std::move(writes), ready, [this, id](Cycles) {
        if (const auto it = sif_dma_done_.find(id); it != sif_dma_done_.end()) {
            it->second = true;
        }
    });
    return id;
}

std::int32_t IopExec::sif_dma_status(std::uint32_t id) const {
    const auto it = sif_dma_done_.find(id);
    return it == sif_dma_done_.end() || it->second ? -1 : 1;
}

// ---- Output
// --------------------------------------------------------------------------------------------------

void IopExec::print(std::string_view text) {
    constexpr std::size_t max_line = 512;
    for (const char c : text) {
        if (c == '\n' || print_line_.size() >= max_line) {
            machine_.trace(TraceCategory::iop, "printf: " + print_line_);
            print_line_.clear();
            if (c == '\n') {
                continue;
            }
        }
        print_line_.push_back(c);
    }
}

} // namespace dq8::rt
