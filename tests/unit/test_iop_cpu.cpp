// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/iop/cpu/iop_cpu.h"
#include "runtime/iop/hw/iop_hardware.h"
#include "support/irx_builder.h"
#include "support/mips_asm.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

namespace rt = dq8::rt;
namespace mips = dq8::test::mips;
namespace ia = dq8::test::iopasm;
using mips::Reg;

namespace {

// Device registers at 0x1F801000 and up answer from a map; everything else is a bus error.
struct FakeBus final : rt::IIopCpuBus {
    bool read(std::uint32_t phys, std::uint32_t width, std::uint32_t& value, std::uint32_t) override {
        if (phys < 0x1F801000u || phys >= 0x1F802000u) {
            return false;
        }
        value = regs[phys] & (width == 4 ? 0xFFFFFFFFu : (1u << (8 * width)) - 1);
        ++reads;
        return true;
    }
    bool write(std::uint32_t phys, std::uint32_t, std::uint32_t value, std::uint32_t) override {
        if (phys < 0x1F801000u || phys >= 0x1F802000u) {
            return false;
        }
        regs[phys] = value;
        return true;
    }
    std::optional<std::uint32_t> hle_slot(std::uint32_t phys) const override {
        const std::uint32_t off = phys - rt::iop_map::hle_page;
        if (phys >= rt::iop_map::hle_page && off < 8 * 16 && off % 8 == 0) {
            return off / 8;
        }
        return std::nullopt;
    }
    std::map<std::uint32_t, std::uint32_t> regs;
    int reads = 0;
};

struct Cpu {
    Cpu() : ram(rt::iop_ram_bytes, 0), cpu(ram.data(), bus) { s.pc = base; }

    void program(std::vector<std::uint32_t> words) {
        std::memcpy(ram.data() + base, words.data(), words.size() * 4);
    }
    std::uint32_t r32(std::uint32_t at) const {
        std::uint32_t v = 0;
        std::memcpy(&v, ram.data() + at, 4);
        return v;
    }
    void w32(std::uint32_t at, std::uint32_t v) { std::memcpy(ram.data() + at, &v, 4); }
    // Runs at most `n` instructions.
    rt::IopCpuExit run(std::uint64_t n) {
        now = 0;
        const rt::Cycles limit = n * 8;
        return cpu.run(s, now, limit, 8, stop);
    }

    static constexpr std::uint32_t base = 0x00010000;
    std::vector<std::uint8_t> ram;
    FakeBus bus;
    rt::IopCpu cpu;
    rt::IopCpuState s;
    rt::IopCpuStop stop;
    rt::Cycles now = 0;
};

} // namespace

TEST_CASE("iop cpu: integer arithmetic, shifts, compares and hi/lo", "[iop][cpu]") {
    Cpu c;
    c.program({
        mips::li(mips::t0, -5),
        mips::li(mips::t1, 3),
        mips::addu(mips::t2, mips::t0, mips::t1), // -2
        mips::subu(mips::t3, mips::t1, mips::t0), // 8
        ia::sra(mips::t4, mips::t0, 1),           // -3
        ia::srl(mips::t5, mips::t0, 28),          // 0xF
        mips::slt(mips::t6, mips::t0, mips::t1),  // 1
        ia::sltu(mips::t7, mips::t0, mips::t1),   // 0
        ia::mult(mips::t0, mips::t1),             // -15
        ia::mflo(mips::s0),
        ia::mfhi(mips::s1),
        ia::div(mips::t0, mips::t1), // -1 rem -2
        ia::mflo(mips::s2),
        ia::mfhi(mips::s3),
        ia::nor(mips::s4, mips::zero, mips::zero), // ~0
        ia::xori(mips::s5, mips::t1, 0xFF),        // 0xFC
        mips::lui(mips::s6, 0x8000),
        ia::divu(mips::t1, mips::zero), // by zero: lo all ones, hi the dividend
        ia::mflo(mips::s7),
        ia::mfhi(mips::v1),
    });
    REQUIRE(c.run(20) == rt::IopCpuExit::limit);
    CHECK(c.s.gpr[mips::t2] == static_cast<std::uint32_t>(-2));
    CHECK(c.s.gpr[mips::t3] == 8);
    CHECK(c.s.gpr[mips::t4] == static_cast<std::uint32_t>(-3));
    CHECK(c.s.gpr[mips::t5] == 0xF);
    CHECK(c.s.gpr[mips::t6] == 1);
    CHECK(c.s.gpr[mips::t7] == 0);
    CHECK(c.s.gpr[mips::s0] == static_cast<std::uint32_t>(-15));
    CHECK(c.s.gpr[mips::s1] == 0xFFFFFFFFu);
    CHECK(c.s.gpr[mips::s2] == static_cast<std::uint32_t>(-1));
    CHECK(c.s.gpr[mips::s3] == static_cast<std::uint32_t>(-2));
    CHECK(c.s.gpr[mips::s4] == 0xFFFFFFFFu);
    CHECK(c.s.gpr[mips::s5] == 0xFC);
    CHECK(c.s.gpr[mips::s6] == 0x80000000u);
    CHECK(c.s.gpr[mips::s7] == 0xFFFFFFFFu);
    CHECK(c.s.gpr[mips::v1] == 3);
    CHECK(c.now == 20 * 8);
    CHECK(c.cpu.instructions() == 20);
}

TEST_CASE("iop cpu: branch delay slots and links", "[iop][cpu]") {
    Cpu c;
    const std::uint32_t b = Cpu::base;
    c.program({
        mips::li(mips::t0, 1),                // +0
        mips::beq(mips::zero, mips::zero, 2), // +4: to +16
        mips::li(mips::t1, 2),                // +8: delay slot runs
        mips::li(mips::t2, 3),                // +12: skipped
        mips::jal(b + 0x20),                  // +16
        mips::li(mips::t3, 4),                // +20: delay slot
        mips::li(mips::t4, 5),                // +24: after the return
        mips::nop(),                          // +28
        mips::jr(mips::ra),                   // +32
        mips::li(mips::t5, 6),                // +36
    });
    REQUIRE(c.run(9) == rt::IopCpuExit::limit);
    CHECK(c.s.gpr[mips::t1] == 2);
    CHECK(c.s.gpr[mips::t2] == 0);
    CHECK(c.s.gpr[mips::t3] == 4);
    CHECK(c.s.gpr[mips::t5] == 6);
    CHECK(c.s.gpr[mips::ra] == b + 24);
    CHECK(c.s.gpr[mips::t4] == 5);
}

TEST_CASE("iop cpu: a load lands one instruction late", "[iop][cpu]") {
    Cpu c;
    c.w32(0x20000, 0x11111111u);
    c.program({
        mips::lui(mips::a0, 0x0002),
        mips::li(mips::t0, 7),
        mips::lw(mips::t0, 0, mips::a0),
        mips::addu(mips::t1, mips::t0, mips::zero), // the old value
        mips::addu(mips::t2, mips::t0, mips::zero), // the loaded value
        mips::lw(mips::t3, 0, mips::a0),
        mips::li(mips::t3, 9), // a write in the delay slot beats the load
        mips::nop(),
    });
    REQUIRE(c.run(8) == rt::IopCpuExit::limit);
    CHECK(c.s.gpr[mips::t1] == 7);
    CHECK(c.s.gpr[mips::t2] == 0x11111111u);
    CHECK(c.s.gpr[mips::t3] == 9);
}

TEST_CASE("iop cpu: an LWL/LWR pair reads an unaligned word", "[iop][cpu]") {
    Cpu c;
    c.w32(0x20000, 0x44332211u);
    c.w32(0x20004, 0x88776655u);
    c.program({
        mips::lui(mips::a0, 0x0002),
        ia::lwl(mips::t0, 4, mips::a0), // the word at +1: bytes 0x22 0x33 0x44 0x55
        ia::lwr(mips::t0, 1, mips::a0), // in the load delay of the first: merges with it
        mips::nop(),
        ia::lh(mips::t1, 2, mips::a0),
        ia::lhu(mips::t2, 6, mips::a0),
        ia::lb(mips::t3, 7, mips::a0),
        mips::nop(),
    });
    REQUIRE(c.run(8) == rt::IopCpuExit::limit);
    CHECK(c.s.gpr[mips::t0] == 0x55443322u);
    CHECK(c.s.gpr[mips::t1] == 0x4433u);
    CHECK(c.s.gpr[mips::t2] == 0x8877u);
    CHECK(c.s.gpr[mips::t3] == 0xFFFFFF88u);
}

TEST_CASE("iop cpu: stores of every width, and device accesses go to the bus", "[iop][cpu]") {
    Cpu c;
    c.program({
        mips::lui(mips::a0, 0x0002),
        mips::li(mips::t0, 0x1234),
        ia::sh(mips::t0, 2, mips::a0),
        mips::sb(mips::t0, 1, mips::a0),
        mips::lui(mips::a1, 0x1F80),
        mips::ori(mips::a1, mips::a1, 0x1010),
        mips::sw(mips::t0, 0, mips::a1),
        mips::lw(mips::t1, 0, mips::a1),
        mips::nop(),
    });
    REQUIRE(c.run(9) == rt::IopCpuExit::limit);
    CHECK(c.r32(0x20000) == 0x12343400u);
    CHECK(c.bus.regs[0x1F801010u] == 0x1234u);
    CHECK(c.s.gpr[mips::t1] == 0x1234u);
}

TEST_CASE("iop cpu: faults end the run with what, where and whether in a delay slot", "[iop][cpu]") {
    SECTION("overflow") {
        Cpu c;
        c.program({mips::lui(mips::t0, 0x7FFF), mips::ori(mips::t0, mips::t0, 0xFFFF),
                   ia::addi(mips::t1, mips::t0, 1)});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::overflow);
        CHECK(c.stop.pc == Cpu::base + 8);
        CHECK(c.s.gpr[mips::t1] == 0);
        CHECK(rt::iop_exception_code(c.stop.fault) == 12);
    }
    SECTION("misaligned load") {
        Cpu c;
        c.program({mips::li(mips::a0, 2), mips::lw(mips::t0, 0, mips::a0)});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::load_address);
        CHECK(c.stop.address == 2);
    }
    SECTION("break in a delay slot") {
        Cpu c;
        c.program({mips::b(4), mips::break_(7)});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::break_);
        CHECK(c.stop.delay_slot);
        CHECK(c.stop.pc == Cpu::base + 4);
    }
    SECTION("COP0 is reserved") {
        Cpu c;
        c.program({ia::mfc0(mips::t0, 12)});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::reserved);
        CHECK(c.stop.instruction == ia::mfc0(mips::t0, 12));
    }
    SECTION("syscall") {
        Cpu c;
        c.program({mips::syscall()});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::syscall);
    }
    SECTION("a load from an unmapped address") {
        Cpu c;
        c.program({mips::lui(mips::a0, 0x1F40), mips::lw(mips::t0, 0, mips::a0)});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::load_bus);
        CHECK(c.stop.address == 0x1F400000u);
    }
    SECTION("a jump outside RAM") {
        Cpu c;
        c.program({mips::lui(mips::t0, 0x1FC0), mips::jr(mips::t0), mips::nop()});
        REQUIRE(c.run(10) == rt::IopCpuExit::fault);
        CHECK(c.stop.fault == rt::IopFault::fetch_bus);
        CHECK(c.stop.pc == 0x1FC00000u);
    }
}

TEST_CASE("iop cpu: a fetch from the HLE page is a trap at that slot", "[iop][cpu]") {
    Cpu c;
    c.w32(0x20000, 0xCAFEu);
    c.program({
        mips::lui(mips::a1, 0x0002), mips::jal(rt::iop_map::hle_page + 3 * 8),
        mips::lw(mips::a0, 0, mips::a1), // the delay slot's load lands before the HLE call
    });
    REQUIRE(c.run(10) == rt::IopCpuExit::trap);
    CHECK(c.stop.trap == 3);
    CHECK(c.s.pc == rt::iop_map::hle_page + 3 * 8);
    CHECK(c.s.gpr[mips::ra] == Cpu::base + 12);
    CHECK(c.s.gpr[mips::a0] == 0xCAFEu);
    CHECK_FALSE(c.s.load_pending);
    CHECK(c.cpu.instructions() == 3);
}

TEST_CASE("iop cpu: the limit can be lowered while running", "[iop][cpu]") {
    Cpu c;
    c.program({mips::b(-1), mips::nop()});
    c.now = 0;
    rt::Cycles limit = 800;
    REQUIRE(c.cpu.run(c.s, c.now, limit, 8, c.stop) == rt::IopCpuExit::limit);
    CHECK(c.now == 800);
}
