// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/iso9660.h"
#include "runtime/ee/context.h"
#include "runtime/ee/guest_memory.h"
#include "runtime/io/disc_source.h"
#include "runtime/io/pad_state.h"
#include "runtime/iop/iop.h"
#include "runtime/kernel/kernel_services.h"
#include "runtime/machine/events.h"
#include "runtime/machine/interrupts.h"
#include "support/fake_machine.h"
#include "support/iso_builder.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// A host-side stand-in for the EE's SIF libraries, for IOP tests: it speaks the SIF command and RPC protocol
// the way the game's recompiled libraries do (Docs/milestones/M3-boots.md, "Protocol notes"), through the
// same kernel services (sceSifSetDma, sceSifGetReg, ...) and the same EE memory structures: one fixed receive
// slot that the IOP's packets land in, a DMAC channel 5 interrupt per packet that this client services by
// copying the packet, clearing its size byte, re-arming SIF0 and dispatching it, RPC packets in uncached
// slots, and 0x28-byte client structures whose +0x24 word is the bound server. Time only moves when the
// client waits, by running the FakeMachine's events.
namespace dq8::test {

class SifClient {
  public:
    // EE memory the client uses (synthetic addresses, all in RAM).
    static constexpr std::uint32_t receive_slot = 0x00010000; // 0x80 bytes: where IOP packets land
    static constexpr std::uint32_t command_data = 0x00010100; // what software register 1 points at
    static constexpr std::uint32_t init_packet = 0x00010200;
    static constexpr std::uint32_t descriptors = 0x00010300;  // up to 32 x 16 bytes
    static constexpr std::uint32_t packet_slots = 0x00011000; // 32 x 0x40, used through the uncached alias
    static constexpr std::uint32_t uncached = 0x20000000;
    static constexpr std::uint32_t client_size = 0x28;

    explicit SifClient(FakeMachine& m) : m_(m) { rt::reset_context(ctx_, 0); }

    FakeMachine& machine() { return m_; }

    // ---- kernel calls ----------------------------------------------------------------------------------

    std::uint64_t kcall(std::string_view name, std::initializer_list<std::uint32_t> args) {
        const rt::KernelResolution r = m_.kernel().resolve_name(name);
        if (r.kind != rt::KernelResolution::Kind::native) {
            throw std::runtime_error("no kernel service " + std::string(name));
        }
        unsigned i = 0;
        for (const std::uint32_t a : args) {
            rt::set_gpr_lo(ctx_, i < 4 ? 4 + i : 8 + (i - 4), a);
            ++i;
        }
        rt::KernelCallContext call(m_, ctx_, r.name, name, r.has_number, r.number, r.interrupt_context);
        m_.kernel().invoke(*r.service, call);
        return rt::gpr_lo(ctx_, 2);
    }
    std::uint32_t get_reg(std::uint32_t reg) {
        return static_cast<std::uint32_t>(kcall("sceSifGetReg", {reg}));
    }
    void set_reg(std::uint32_t reg, std::uint32_t value) { kcall("sceSifSetReg", {reg, value}); }

    // ---- EE memory -------------------------------------------------------------------------------------

    void write(std::uint32_t addr, std::span<const std::uint8_t> bytes) {
        m_.memory().write_bytes(addr, bytes);
    }
    std::vector<std::uint8_t> read(std::uint32_t addr, std::size_t n) {
        std::vector<std::uint8_t> out(n);
        m_.memory().read_bytes(addr, out);
        return out;
    }
    void write32(std::uint32_t addr, std::uint32_t v) {
        std::uint8_t b[4];
        std::memcpy(b, &v, 4);
        write(addr, b);
    }
    std::uint32_t read32(std::uint32_t addr) {
        std::uint32_t v = 0;
        const auto b = read(addr, 4);
        std::memcpy(&v, b.data(), 4);
        return v;
    }

    // ---- time --------------------------------------------------------------------------------------------

    // Services SIF0 interrupts and runs events until `done` holds; false when the event queue runs dry or the
    // clock passes `limit` first.
    bool wait_for(const std::function<bool()>& done, rt::Cycles limit = ~rt::Cycles{0}) {
        for (;;) {
            service_interrupts();
            if (done()) {
                return true;
            }
            const auto next = m_.events().next_time();
            if (!next || *next > limit) {
                return false;
            }
            m_.advance(*next);
        }
    }
    // Runs every event up to `t` (servicing interrupts on the way) and moves the clock there.
    void run_until(rt::Cycles t) {
        wait_for([] { return false; }, t);
        m_.advance(t);
        service_interrupts();
    }

    // ---- command layer -----------------------------------------------------------------------------------

    // As the EE's command-layer init does on the "IOP not known yet" path: enable and arm SIF0, wait for the
    // IOP's command-layer flag, learn the IOP command buffer, publish it and our data block in the software
    // registers, and send init (option 0) with our receive slot.
    void init_cmd() {
        if ((m_.interrupts().dmac_stat() & (1u << (16 + 5))) == 0) {
            m_.interrupts().write_dmac_stat(1u << (16 + 5)); // enable channel 5 (a mask write toggles)
        }
        kcall("sceSifSetDChain", {});
        iop_buffer_ = get_reg(0x80000000u);
        if (iop_buffer_ != 0) {
            send_simple(0x80000000u, 0, receive_slot);
            return;
        }
        if (!wait_for([this] { return (get_reg(4) & 0x20000u) != 0; })) {
            throw std::runtime_error("the IOP never raised the command-layer flag");
        }
        iop_buffer_ = get_reg(2);
        set_reg(0x80000000u, iop_buffer_);
        set_reg(0x80000001u, command_data);
        sregs_.fill(0);
        send_simple(0x80000002u, 0, receive_slot);
    }

    // As the EE's RPC-layer init does: the command layer, then (unless software register 2 says it was done)
    // init with option 1 and a wait for the IOP to set our software register 0.
    void init_rpc() {
        init_cmd();
        if (get_reg(0x80000002u) == 0) {
            std::vector<std::uint8_t> p(16, 0);
            send_command(0x80000002u, p, 0, 0, 0, 1);
            if (!wait_for([this] { return sregs_[0] != 0; })) {
                throw std::runtime_error("the IOP never set software register 0");
            }
            set_reg(0x80000002u, 1);
        }
    }

    // Sends `packet` (its header is filled in here) with optional extra data ahead of it; returns the
    // transfer id.
    std::uint32_t send_command(std::uint32_t cid, std::vector<std::uint8_t> packet, std::uint32_t extra_src,
                               std::uint32_t extra_dest, std::uint32_t extra_size, std::uint32_t opt = 0,
                               std::uint32_t packet_addr = 0) {
        put(packet, 0x0, static_cast<std::uint32_t>(packet.size()) | (extra_size << 8));
        put(packet, 0x4, extra_size != 0 ? extra_dest : 0);
        put(packet, 0x8, cid);
        put(packet, 0xC, opt);
        const std::uint32_t at = packet_addr != 0 ? packet_addr : init_packet;
        write(at & 0x1FFFFFFFu, packet);
        std::uint32_t n = 0;
        if (extra_size != 0) {
            write_descriptor(n++, extra_src, extra_dest, extra_size, 0);
        }
        write_descriptor(n++, at, iop_buffer_, static_cast<std::uint32_t>(packet.size()), 0x44);
        return static_cast<std::uint32_t>(kcall("sceSifSetDma", {descriptors, n}));
    }

    // Copies EE bytes into IOP RAM the way the game uploads sound data: one descriptor, attribute 0.
    std::uint32_t dma_to_iop(std::uint32_t ee_src, std::uint32_t iop_dest, std::uint32_t size) {
        write_descriptor(0, ee_src, iop_dest, size, 0);
        return static_cast<std::uint32_t>(kcall("sceSifSetDma", {descriptors, 1}));
    }
    std::int32_t dma_stat(std::uint32_t id) {
        return static_cast<std::int32_t>(kcall("sceSifDmaStat", {id}));
    }

    // The EE's IOP reset: stop SIF0, clear the boot flag, send the reset packet straight to the IOP command
    // buffer, then clear the other two flags and the software registers the next init relies on. Returns the
    // transfer id.
    std::uint32_t reset_iop(std::string_view arg, std::uint32_t mode = 0) {
        kcall("sceSifStopDma", {});
        const std::uint32_t dest = get_reg(0x80000000u);
        std::vector<std::uint8_t> p(0x68, 0);
        put(p, 0x0, 0x68);
        put(p, 0x8, 0x80000003u);
        put(p, 0x10, static_cast<std::uint32_t>(arg.size()));
        put(p, 0x14, mode);
        std::memcpy(p.data() + 0x18, arg.data(), std::min<std::size_t>(arg.size(), 0x50));
        write(init_packet, p);
        set_reg(4, 0x40000);
        write_descriptor(0, init_packet, dest, 0x68, 0x44);
        const auto id = static_cast<std::uint32_t>(kcall("sceSifSetDma", {descriptors, 1}));
        if (id != 0) {
            set_reg(4, 0x10000);
            set_reg(4, 0x20000);
            set_reg(0x80000002u, 0);
            set_reg(0x80000000u, 0);
        }
        return id;
    }
    // The EE's sync loop after a reset.
    bool sync_iop() {
        return wait_for([this] { return (get_reg(4) & 0x40000u) != 0; });
    }

    // ---- RPC ---------------------------------------------------------------------------------------------

    // Binds the 0x28-byte client at `client` to `sid` and waits for the answer; returns the server word.
    std::uint32_t bind(std::uint32_t client, std::uint32_t sid) {
        const std::uint32_t pkt = alloc_packet();
        std::vector<std::uint8_t> clear(client_size, 0);
        write(client, clear);
        write32(client + 0x00, pkt);
        write32(client + 0x04, read32((pkt & 0x1FFFFFFFu) + 0x18));
        write32(client + 0x08, 0xFFFFFFFFu);
        std::vector<std::uint8_t> p = read(pkt & 0x1FFFFFFFu, 0x40);
        put(p, 0x1C, client);
        put(p, 0x20, sid);
        send_command(0x80000009u, p, 0, 0, 0, 0, pkt);
        if (!wait_for([&] { return read32(client) == 0; })) {
            throw std::runtime_error("no answer to a bind");
        }
        return read32(client + 0x24);
    }

    // Calls function `fno` of the bound `client`. With `wait` the call returns once the END arrived; without
    // it the call returns at once and `call_done` tells when it finished. Returns false when it never ended.
    bool call(std::uint32_t client, std::uint32_t fno, std::uint32_t send, std::uint32_t send_size,
              std::uint32_t recv, std::uint32_t recv_size, bool wait = true, bool end_packet = true) {
        const std::uint32_t pkt = alloc_packet();
        write32(client + 0x00, pkt);
        write32(client + 0x04, read32((pkt & 0x1FFFFFFFu) + 0x18));
        ends_[client] = 0;
        std::vector<std::uint8_t> p = read(pkt & 0x1FFFFFFFu, 0x40);
        put(p, 0x1C, client);
        put(p, 0x20, fno);
        put(p, 0x24, send_size);
        put(p, 0x28, recv);
        put(p, 0x2C, recv_size);
        put(p, 0x30, end_packet ? 1 : 0);
        put(p, 0x34, read32(client + 0x24));
        send_command(0x8000000Au, p, send, read32(client + 0x14), send_size, 0, pkt);
        if (!wait) {
            return true;
        }
        return wait_for([&] { return call_done(client); });
    }
    // sceSifCheckStatRpc's test: idle unless the client holds a packet that is still its request and still
    // allocated.
    bool call_done(std::uint32_t client) {
        const std::uint32_t pkt = read32(client) & 0x1FFFFFFFu;
        if (pkt == 0) {
            return true;
        }
        return !(read32(client + 0x04) == read32(pkt + 0x18) && (read32(pkt + 0x10) & 1u) != 0);
    }
    // How many CALL ends the client has seen (its end function would have run this often).
    std::uint32_t ends(std::uint32_t client) { return ends_[client]; }

    // ---- what arrived ------------------------------------------------------------------------------------

    const std::array<std::uint32_t, 32>& ee_sregs() const { return sregs_; }
    const std::vector<std::vector<std::uint8_t>>& received() const { return received_; }
    // fileio completions (system command 0x80000011): the option word of each, in arrival order.
    const std::vector<std::uint32_t>& fileio_completions() const { return fileio_done_; }
    std::uint32_t iop_command_buffer() const { return iop_buffer_; }
    std::uint64_t interrupts_serviced() const { return serviced_; }

    // The EE's SIF0 handler: one packet per interrupt, from the fixed slot.
    void service_interrupts() {
        while ((m_.interrupts().dmac_pending_channels() & (1u << 5)) != 0) {
            m_.interrupts().write_dmac_stat(1u << 5);
            ++serviced_;
            const auto size = read(receive_slot, 1)[0];
            if (size == 0) {
                continue; // nothing there: return without re-arming
            }
            write(receive_slot, std::vector<std::uint8_t>{0});
            std::vector<std::uint8_t> packet = read(receive_slot, (size + 15u) & ~15u);
            packet[0] = static_cast<std::uint8_t>(size);
            kcall("isceSifSetDChain", {});
            received_.push_back(packet);
            dispatch(packet);
        }
    }

    static void put(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
        if (b.size() < off + 4) {
            b.resize(off + 4);
        }
        std::memcpy(b.data() + off, &v, 4);
    }
    static std::uint32_t get(const std::vector<std::uint8_t>& b, std::size_t off) {
        std::uint32_t v = 0;
        if (off + 4 <= b.size()) {
            std::memcpy(&v, b.data() + off, 4);
        }
        return v;
    }

  private:
    void send_simple(std::uint32_t cid, std::uint32_t opt, std::uint32_t word) {
        std::vector<std::uint8_t> p(0x14, 0);
        put(p, 0x10, word);
        send_command(cid, p, 0, 0, 0, opt);
    }

    void write_descriptor(std::uint32_t i, std::uint32_t src, std::uint32_t dest, std::uint32_t size,
                          std::uint32_t attr) {
        const std::uint32_t at = descriptors + 16 * i;
        write32(at + 0x0, src);
        write32(at + 0x4, dest);
        write32(at + 0x8, size);
        write32(at + 0xC, attr);
    }

    // A free packet slot, returned as its uncached address with rec_id, address and rpc id filled in.
    std::uint32_t alloc_packet() {
        for (std::uint32_t i = 0; i < 32; ++i) {
            const std::uint32_t at = packet_slots + i * 0x40;
            if ((read32(at + 0x10) & 1u) == 0) {
                std::vector<std::uint8_t> p(0x40, 0);
                put(p, 0x10, (i << 16) | 5u);
                put(p, 0x14, at | uncached);
                if (++rpc_id_ == 0) {
                    rpc_id_ = 1;
                }
                put(p, 0x18, rpc_id_);
                write(at, p);
                return at | uncached;
            }
        }
        throw std::runtime_error("no free RPC packet slot");
    }
    void free_packet(std::uint32_t pkt) {
        const std::uint32_t at = pkt & 0x1FFFFFFFu;
        write32(at + 0x18, 0);
        write32(at + 0x10, read32(at + 0x10) & ~1u);
    }

    void dispatch(const std::vector<std::uint8_t>& p) {
        const std::uint32_t cid = get(p, 0x8);
        switch (cid) {
        case 0x80000000u: // the IOP moved its command buffer
            iop_buffer_ = get(p, 0x10);
            break;
        case 0x80000001u: // set one of our software registers
            if (get(p, 0x10) < sregs_.size()) {
                sregs_[get(p, 0x10)] = get(p, 0x14);
            }
            break;
        case 0x80000008u: { // RPC end
            const std::uint32_t client = get(p, 0x1C);
            const std::uint32_t ended = get(p, 0x20);
            if (ended == 0x80000009u) {
                write32(client + 0x24, get(p, 0x24));
                write32(client + 0x14, get(p, 0x28));
            } else if (ended == 0x8000000Au) {
                ++ends_[client];
            }
            free_packet(read32(client));
            write32(client, 0);
            break;
        }
        case 0x80000011u: // a fileio completion record is in EE memory
            fileio_done_.push_back(get(p, 0xC));
            break;
        default:
            unknown_.push_back(cid);
            break;
        }
    }

    FakeMachine& m_;
    R5900Context ctx_{};
    std::uint32_t iop_buffer_ = 0;
    std::uint32_t rpc_id_ = 0;
    std::array<std::uint32_t, 32> sregs_{};
    std::vector<std::vector<std::uint8_t>> received_;
    std::vector<std::uint32_t> fileio_done_;
    std::vector<std::uint32_t> unknown_;
    std::map<std::uint32_t, std::uint32_t> ends_;
    std::uint64_t serviced_ = 0;
};

// ---- a ready-made IOP under test
// ----------------------------------------------------------------------------

// Pad input from a table: the state for (port, vblank), else `fallback`.
class TableInput final : public rt::IInputSource {
  public:
    rt::PadState sample(unsigned port, std::uint64_t vblank) override {
        ++samples;
        const auto it = states.find({port, vblank});
        return it != states.end() ? it->second : fallback;
    }
    std::map<std::pair<unsigned, std::uint64_t>, rt::PadState> states;
    rt::PadState fallback;
    std::uint64_t samples = 0;
};

// The module table the tests use: a profile in the shape of the game's [iop] table, with synthetic files.
inline std::vector<rt::IopModuleSpec> test_modules() {
    return {
        {"SIO2MAN", {}},
        {"PADMAN", {"padman.v1"}},
        {"MCSERV", {"mcserv.v1"}},
        {"SDRDRV", {"null:0x80000701"}},
        {"PCMPLAY", {"null:0x123456"}},
    };
}

// A synthetic disc: the update image, one file per module, a title-like file and a multi-sector data file.
inline IsoBuilder test_disc() {
    IsoBuilder b;
    b.add_file("/MODULES/IOPRP.IMG", seeded_bytes(1, 3000));
    for (const char* name : {"SIO2MAN", "PADMAN", "MCSERV", "SDRDRV", "PCMPLAY", "EXTRA"}) {
        b.add_file(std::string("/MODULES/") + name + ".IRX", seeded_bytes(2, 700));
    }
    b.add_file("/BIN/TITLE.BIN", seeded_bytes(3, 5000));
    b.add_file("/DATA.DAT", seeded_bytes(4, 10 * 2048 + 100));
    b.add_file("/DATA.HD6", seeded_bytes(5, 333));
    return b;
}

struct IopRig {
    explicit IopRig(const IsoBuilder& disc = test_disc(),
                    std::vector<rt::IopModuleSpec> modules = test_modules(),
                    std::string reset_image = "MODULES/IOPRP.IMG")
        : image(core::iso::Image::from_bytes(disc.build(&layout))), source(rt::make_iso_disc_source(image)),
          iop(std::make_unique<rt::Iop>(
              rt::IopConfig{source.get(), &input, std::move(modules), std::move(reset_image)})),
          client(m) {
        m.attach(*iop);
    }

    Layout layout;
    core::iso::Image image;
    std::unique_ptr<rt::IDiscSource> source;
    TableInput input;
    FakeMachine m;
    std::unique_ptr<rt::Iop> iop;
    SifClient client;
};

} // namespace dq8::test
