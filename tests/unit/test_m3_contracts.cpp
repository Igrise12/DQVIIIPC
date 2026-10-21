// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Every M3 contract header compiles on its own and the shared pieces fit together
// (Docs/milestones/M3-boots.md).
#include "dq8/gs/gs_parallel_abi.h"
#include "runtime/ee/overrides.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_pipeline.h"
#include "runtime/gs/gs_sink.h"
#include "runtime/gs/priv_regs.h"
#include "runtime/io/disc_source.h"
#include "runtime/io/pad_state.h"
#include "runtime/iop/iop.h"
#include "runtime/iop/iop_memory.h"
#include "runtime/iop/iop_server.h"
#include "runtime/kernel/fiber.h"
#include "runtime/kernel/kernel_layout.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/machine/ee_devices.h"
#include "runtime/machine/machine.h"
#include "runtime/platform/platform.h"
#include "support/fake_machine.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace dq8;

namespace {

struct CountingListener final : rt::IVblankListener {
    std::vector<std::uint64_t> starts;
    std::vector<std::uint64_t> ends;
    std::vector<int>* order = nullptr;
    int id = 0;
    void on_vblank_start(std::uint64_t field) override {
        starts.push_back(field);
        if (order) {
            order->push_back(id);
        }
    }
    void on_vblank_end(std::uint64_t field) override { ends.push_back(field); }
};

struct NullDevice final : rt::IDevice {
    int attached = 0;
    int resets = 0;
    std::string_view name() const override { return "null"; }
    void attach(rt::MachineServices& m) override {
        ++attached;
        m.kernel().add({"GetMemorySize", [](rt::KernelCallContext& c) { c.ret(32 << 20); }, 0, {}, "null"});
    }
    void reset() override { ++resets; }
};

} // namespace

TEST_CASE("m3 contracts: GS privileged page layout", "[m3][contracts]") {
    static_assert(gs::priv_page_size == 8192);
    gs::PrivRegs regs;
    regs.write64(gs::PrivReg::csr, gs::csr_field | gs::csr_vsint);
    CHECK(regs.read64(gs::PrivReg::csr) == (gs::csr_field | gs::csr_vsint));
    CHECK(regs.page[0x1000] == static_cast<std::uint8_t>(gs::csr_vsint));
    CHECK(regs.page[0x1001] == 0x20); // FIELD is bit 13
    regs.write64(gs::PrivReg::bgcolor, 0x112233);
    CHECK(regs.read64(gs::PrivReg::bgcolor) == 0x112233);
}

TEST_CASE("m3 contracts: kernel RAM layout sits below the user area", "[m3][contracts]") {
    static_assert(rt::hle_kernel_ram_end == 0x00100000u);
    static_assert(rt::hle_handler_stack_top % 16 == 0);
    static_assert(rt::hle_handler_stack_base >= rt::hle_kernel_ram_base);
    CHECK(rt::in_hle_kernel_ram(0x000FFFF0));
    CHECK_FALSE(rt::in_hle_kernel_ram(0x00100000));
    CHECK_FALSE(rt::in_hle_kernel_ram(0x000DFFFC));
}

TEST_CASE("m3 contracts: pad buttons follow the DualShock 2 byte order", "[m3][contracts]") {
    CHECK(rt::pad_select == 0x0001);
    CHECK(rt::pad_start == 0x0008);
    CHECK(rt::pad_left == 0x0080);
    CHECK(rt::pad_cross == 0x4000);
    CHECK(rt::pad_square == 0x8000);
    const rt::PadState idle;
    CHECK(idle.connected);
    CHECK(idle.lx == 0x80);
}

TEST_CASE("m3 contracts: the fake machine attaches devices and runs vblank phases in order",
          "[m3][contracts]") {
    test::FakeMachine m;
    NullDevice dev;
    m.attach(dev);
    CHECK(dev.attached == 1);
    CHECK(dev.resets == 1);
    CHECK(m.kernel().find("GetMemorySize") != nullptr);

    std::vector<int> order;
    CountingListener intc, gs_l, iop;
    intc.order = gs_l.order = iop.order = &order;
    intc.id = 3;
    gs_l.id = 0;
    iop.id = 2;
    m.add_vblank_listener(rt::VblankPhase::intc, intc);
    m.add_vblank_listener(rt::VblankPhase::gs, gs_l);
    m.add_vblank_listener(rt::VblankPhase::iop, iop);
    m.fire_vblank_start();
    m.fire_vblank_end();
    m.fire_vblank_start();
    CHECK(order == std::vector<int>{0, 2, 3, 0, 2, 3});
    CHECK(gs_l.starts == std::vector<std::uint64_t>{0, 1});
    CHECK(gs_l.ends == std::vector<std::uint64_t>{0});
    CHECK(m.vblank_count() == 2);

    int fired = 0;
    m.events().schedule(1000, "test", [&](rt::Cycles) { ++fired; });
    m.note_poll(0x100, 0x12001000, 0);
    for (int i = 0; i < 63; ++i) {
        m.note_poll(0x100, 0x12001000, 0);
    }
    CHECK(m.spins == 1);
    CHECK(fired == 1);
    CHECK(m.clock().now() == 1000);

    m.emit(rt::MachineEvent{"thread", {}}.add("id", std::int64_t{1}).add("state", std::string("start")));
    REQUIRE(m.emitted.size() == 1);
    CHECK(m.emitted[0].fields.size() == 2);
}

TEST_CASE("m3 contracts: interfaces are abstract where sessions implement them", "[m3][contracts]") {
    STATIC_CHECK(std::is_abstract_v<rt::Fiber>);
    STATIC_CHECK(std::is_abstract_v<rt::Scheduler>);
    STATIC_CHECK(std::is_abstract_v<gs::IGsBackend>);
    STATIC_CHECK(std::is_abstract_v<gs::IGsSink>);
    STATIC_CHECK(std::is_abstract_v<rt::IDiscSource>);
    STATIC_CHECK(std::is_abstract_v<rt::IInputSource>);
    STATIC_CHECK(std::is_abstract_v<rt::IopRpcServer>);
    STATIC_CHECK(std::is_abstract_v<rt::IPlatform>);
    STATIC_CHECK(std::is_abstract_v<rt::IMmioDevice>);
    STATIC_CHECK_FALSE(std::is_abstract_v<rt::Machine>);
    STATIC_CHECK(DQ8_GSP_ABI_VERSION == 1u);
    STATIC_CHECK(sizeof(dq8_gsp_options) >= 16);
}
