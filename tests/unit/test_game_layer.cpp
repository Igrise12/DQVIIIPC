// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The game layer (src/game) against a synthetic region profile: the overlay residency hook (ADR-0012), the
// movie policies, the mode watcher, [iop] -> IopModuleSpec and the --until conditions. Every address, size,
// name and byte is ours.
#include "dq8/core/hash.h"
#include "game/iop_modules.h"
#include "game/mode_watcher.h"
#include "game/movies.h"
#include "game/overlay_residency.h"
#include "game/until.h"
#include "runtime/ee/context.h"
#include "runtime/ee/dispatcher.h"
#include "runtime/ee/overlay_manager.h"
#include "support/fake_machine.h"
#include "support/mwo3_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace dq8;
using rt::hex32;

constexpr std::uint32_t main_base = 0x00100000u;
constexpr std::uint32_t loader_hook = 0x00100020u;
constexpr std::uint32_t movie_entry = 0x00100040u;
constexpr std::uint32_t window_base = 0x00300000u;
constexpr std::uint32_t window_end = 0x00310000u;
constexpr std::uint32_t mode_variable = 0x00200000u;
constexpr std::uint32_t caller_ra = 0x00100104u;

// Distinct functions (with different bodies, so identical-code folding cannot merge them): a slot holding
// the same function as a lower one is a resume entry, not an entry.
int movie_calls = 0;
void guest_fn(std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ctx->pc = rt::gpr_u32(*ctx, 31);
}
void guest_movie_fn(std::uint8_t*, R5900Context* ctx, PS2Runtime*) {
    ++movie_calls;
    ctx->pc = rt::gpr_u32(*ctx, 31);
}

test::Mwo3Spec alpha_spec() {
    test::Mwo3Spec s;
    s.overlay_id = 2;
    s.load_address = window_base;
    s.name = "alpha";
    s.seed = 7;
    return s;
}

core::ProfileOverlay profile_overlay(const test::Mwo3Spec& s, std::string name, std::uint32_t index,
                                     const std::vector<std::uint8_t>& container) {
    core::ProfileOverlay o;
    o.name = std::move(name);
    o.id = s.overlay_id;
    o.index = index;
    o.path = "BIN/" + o.name + ".BIN";
    o.text_size = s.text_size;
    o.data_size = s.data_size;
    o.bss_size = s.bss_size;
    o.table_entries = s.table_entries;
    o.segment_memsz = 64 + s.text_size + s.data_size + s.bss_size;
    o.size = container.size();
    o.sha1 = core::to_hex(core::Sha1::of(container));
    return o;
}

struct Fixture {
    std::vector<std::uint8_t> alpha = test::build_mwo3(alpha_spec());
    core::RegionProfile profile;
    std::vector<dq8_guest_fn> main_slots = std::vector<dq8_guest_fn>(0x100 / 4, nullptr);
    std::vector<dq8_guest_fn> window_slots =
        std::vector<dq8_guest_fn>((window_end - window_base) / 4, nullptr);
    rt::OverlayManager overlays{rt::OverlayWindowSpec{window_base, window_end}};
    test::FakeMachine machine;
    std::unique_ptr<rt::Dispatcher> dispatcher;
    alignas(16) R5900Context ctx{};

    Fixture() {
        profile.id = "TEST_00001";
        auto& w = profile.overlay_window;
        w.base = window_base;
        w.end = window_end;
        w.container = "MWo3";
        w.header_size = 64;
        w.code_offset = 64;
        w.loader_hook = loader_hook;
        w.mode_variable = mode_variable;
        profile.overlays.push_back(profile_overlay(alpha_spec(), "ALPHA", 0, alpha));
        test::Mwo3Spec beta = alpha_spec();
        beta.overlay_id = 5;
        profile.overlays.push_back(profile_overlay(beta, "BETA", 1, test::build_mwo3(beta)));
        profile.movie.player_entries = {movie_entry};
        profile.iop.reset_image = "MODULES/RESET.IMG";
        profile.iop.modules = {{"PADMAN", {"padman.v1"}}, {"SOUND", {}}, {"PCM", {"null:0x00123456"}}};

        main_slots[(loader_hook - main_base) / 4] = guest_fn;
        main_slots[(movie_entry - main_base) / 4] = guest_movie_fn;
        window_slots[0x40 / 4] = guest_fn;
        const rt::DispatchTable window_table(window_base, window_end, window_slots);
        overlays.register_unit(2, "alpha", window_table);
        overlays.register_unit(5, "beta", window_table);
        dispatcher = std::make_unique<rt::Dispatcher>(overlays);
        dispatcher->set_main("main", rt::DispatchTable(main_base, main_base + 0x100, main_slots));
        dispatcher->set_machine(&machine);
        rt::reset_context(ctx, main_base);
    }

    void call(std::uint32_t addr, std::uint32_t a0) {
        rt::set_gpr_lo(ctx, 4, a0);
        rt::set_gpr_lo(ctx, 31, caller_ra);
        ctx.pc = addr;
        dispatcher->lookup(addr)(machine.memory().rdram(), &ctx, nullptr);
    }
    std::string stop_message(std::uint32_t addr, std::uint32_t a0) {
        try {
            call(addr, a0);
        } catch (const rt::GuestStop& s) {
            return s.info().message;
        }
        return "<no stop>";
    }
};

const rt::MachineEvent::Value* field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("residency hook: the loaded header makes its overlay resident and returns", "[game]") {
    Fixture f;
    game::OverlayResidency hook({&f.profile, &f.overlays, false});
    f.machine.attach(hook);
    f.dispatcher->install_override(hook.hook_override());

    f.machine.memory().write_bytes(window_base, f.alpha);
    f.call(loader_hook, window_base);
    REQUIRE(f.overlays.resident() != nullptr);
    CHECK(f.overlays.resident()->name == "alpha");
    CHECK(f.ctx.pc == caller_ra);
    CHECK(hook.loads() == 1);
    REQUIRE(f.machine.emitted.size() == 1);
    const rt::MachineEvent& e = f.machine.emitted[0];
    CHECK(e.type == "overlay_resident");
    CHECK(std::get<std::string>(*field(e, "name")) == "ALPHA");
    CHECK(std::get<std::string>(*field(e, "unit")) == "alpha");
    CHECK(std::get<std::uint64_t>(*field(e, "overlay_id")) == 2);
    const auto trigger = game::trigger_from_event(e);
    REQUIRE(trigger);
    CHECK(*trigger == rt::InputTrigger{"overlay", "ALPHA"});
}

TEST_CASE("residency hook: mismatches with the profile stop the guest", "[game]") {
    Fixture f;
    game::OverlayResidency hook({&f.profile, &f.overlays, true});
    f.machine.attach(hook);
    f.dispatcher->install_override(hook.hook_override());

    SECTION("a destination other than the window") {
        f.machine.memory().write_bytes(window_base, f.alpha);
        CHECK(f.stop_message(loader_hook, window_base + 0x40).find("is at 0x00300000") != std::string::npos);
    }
    SECTION("an id the profile does not list") {
        std::vector<std::uint8_t> bytes = f.alpha;
        test::put_le32(bytes, 4, 9);
        f.machine.memory().write_bytes(window_base, bytes);
        CHECK(f.stop_message(loader_hook, window_base).find("overlay id 9") != std::string::npos);
    }
    SECTION("a declared size other than the profile's") {
        f.profile.overlays[0].bss_size += 0x10;
        f.machine.memory().write_bytes(window_base, f.alpha);
        const std::string m = f.stop_message(loader_hook, window_base);
        CHECK(m.find("bss_size 0x00000040, the profile says 0x00000050") != std::string::npos);
    }
    SECTION("loaded bytes that do not hash to the profile's value") {
        std::vector<std::uint8_t> bytes = f.alpha;
        bytes[100] ^= 0xFF;
        f.machine.memory().write_bytes(window_base, bytes);
        CHECK(f.stop_message(loader_hook, window_base).find("hash to") != std::string::npos);
    }
    SECTION("text that changes after the load (--verify-overlays)") {
        f.machine.memory().write_bytes(window_base, f.alpha);
        f.call(loader_hook, window_base);
        CHECK_NOTHROW(f.machine.fire_vblank_start());
        const std::uint8_t poke[1] = {0x5A};
        f.machine.memory().write_bytes(window_base + 64 + 8, poke);
        CHECK_THROWS_AS(f.machine.fire_vblank_start(), rt::GuestStop);
    }
    CHECK(rt::to_string(rt::StopReason::unsupported) == "unsupported");
}

TEST_CASE("residency hook: an overlay with no registered unit stops", "[game]") {
    Fixture f;
    rt::OverlayManager empty{rt::OverlayWindowSpec{window_base, window_end}};
    rt::Dispatcher d(empty);
    d.set_main("main", rt::DispatchTable(main_base, main_base + 0x100, f.main_slots));
    d.set_machine(&f.machine);
    game::OverlayResidency hook({&f.profile, &empty, false});
    f.machine.attach(hook);
    f.dispatcher.reset(); // one dispatcher with overrides per process
    d.install_override(hook.hook_override());
    f.machine.memory().write_bytes(window_base, f.alpha);
    rt::set_gpr_lo(f.ctx, 4, window_base);
    rt::set_gpr_lo(f.ctx, 31, caller_ra);
    f.ctx.pc = loader_hook;
    try {
        d.lookup(loader_hook)(nullptr, &f.ctx, nullptr);
        FAIL("expected a stop");
    } catch (const rt::GuestStop& s) {
        CHECK(s.info().message.find("no code unit registered") != std::string::npos);
    }
}

TEST_CASE("movies: stop names the entry, skip returns with the placeholder", "[game]") {
    CHECK(game::parse_movie_policy("skip") == game::MoviePolicy::skip);
    CHECK(game::parse_movie_policy("stop") == game::MoviePolicy::stop);
    CHECK_FALSE(game::parse_movie_policy("play"));
    {
        Fixture f;
        for (rt::OverrideSpec& s : game::movie_overrides(f.profile, game::MoviePolicy::stop)) {
            f.dispatcher->install_override(std::move(s));
        }
        const std::string m = f.stop_message(movie_entry, 0x1234);
        CHECK(m.find("movie player entry 0x00100040") != std::string::npos);
        CHECK(m.find("a0=0x00001234") != std::string::npos);
    }
    {
        Fixture f;
        const auto specs = game::movie_overrides(f.profile, game::MoviePolicy::skip);
        REQUIRE(specs.size() == 1);
        CHECK(specs[0].name == "movie-skip-0");
        for (const rt::OverrideSpec& s : specs) {
            f.dispatcher->install_override(s);
        }
        rt::set_gpr_lo(f.ctx, 2, 0xFFFF);
        f.call(movie_entry, 0);
        CHECK(f.ctx.pc == caller_ra);
        CHECK(movie_calls == 0); // replaced: the guest player never ran
        CHECK(rt::gpr_lo(f.ctx, 2) == static_cast<std::uint64_t>(game::movie_skip_placeholder_result));
        REQUIRE(f.machine.emitted.size() == 1);
        CHECK(f.machine.emitted[0].type == "movie_skip");
        CHECK(std::get<bool>(*field(f.machine.emitted[0], "placeholder")));
    }
    core::RegionProfile none;
    CHECK(game::movie_overrides(none, game::MoviePolicy::skip).empty());
}

TEST_CASE("mode watcher: one event per change of the mode word, read at vblank start", "[game]") {
    test::FakeMachine machine;
    game::ModeWatcher watcher(mode_variable);
    machine.attach(watcher);
    const auto set_mode = [&](std::uint32_t v) {
        const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                                   static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
        machine.memory().write_bytes(mode_variable, b);
    };
    machine.fire_vblank_start();
    CHECK(machine.emitted.empty());
    set_mode(4);
    machine.fire_vblank_start();
    machine.fire_vblank_start();
    set_mode(11);
    machine.fire_vblank_start();
    REQUIRE(machine.emitted.size() == 2);
    CHECK(std::get<std::int64_t>(*field(machine.emitted[0], "mode")) == 4);
    CHECK(std::get<std::int64_t>(*field(machine.emitted[0], "previous")) == 0);
    CHECK(std::get<std::int64_t>(*field(machine.emitted[1], "mode")) == 11);
    CHECK(watcher.mode() == 11);
    CHECK(watcher.changes() == 2);
    CHECK(*game::trigger_from_event(machine.emitted[0]) == rt::InputTrigger{"mode", "4"});
    CHECK_THROWS_AS(game::ModeWatcher(0x2), std::invalid_argument);
}

TEST_CASE("iop: the profile's [iop] rows become module specs in load order", "[game]") {
    Fixture f;
    const auto specs = game::iop_module_specs(f.profile);
    REQUIRE(specs.size() == 3);
    CHECK(specs[0].name == "PADMAN");
    CHECK(specs[0].services == std::vector<std::string>{"padman.v1"});
    CHECK(specs[1].services.empty());
    CHECK(specs[2].services == std::vector<std::string>{"null:0x00123456"});
    CHECK(game::iop_reset_image(f.profile) == "MODULES/RESET.IMG");
}

TEST_CASE("until: conditions parse against the profile and track events", "[game]") {
    Fixture f;
    CHECK(game::parse_until("vblank:600", &f.profile).vblank == 600);
    CHECK(game::parse_until("overlay:alpha", &f.profile).trigger == rt::InputTrigger{"overlay", "ALPHA"});
    CHECK(game::parse_until("overlay:anything", nullptr).trigger.value == "ANYTHING");
    CHECK(game::parse_until("mode:0x4", &f.profile).trigger == rt::InputTrigger{"mode", "4"});
    CHECK_THROWS_AS(game::parse_until("overlay:gamma", &f.profile), std::invalid_argument);
    CHECK_THROWS_AS(game::parse_until("vblank:", &f.profile), std::invalid_argument);
    CHECK_THROWS_AS(game::parse_until("rpc_bind:1", &f.profile), std::invalid_argument);
    CHECK_THROWS_AS(game::parse_until("soon", &f.profile), std::invalid_argument);
    try {
        game::parse_until("title", &f.profile);
        FAIL("title needs a marker");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("[title] marker") != std::string::npos);
    }

    game::UntilTracker mode(game::parse_until("mode:4", nullptr));
    mode.observe(rt::MachineEvent{"mode", {}}.add("mode", std::int64_t{1}), 10);
    CHECK_FALSE(mode.met());
    mode.observe(rt::MachineEvent{"mode", {}}.add("mode", std::int64_t{4}), 12);
    mode.observe(rt::MachineEvent{"mode", {}}.add("mode", std::int64_t{4}), 20);
    CHECK(mode.met_at() == 12u);

    game::UntilTracker vblank(game::parse_until("vblank:5", nullptr));
    vblank.at_vblank(4);
    CHECK_FALSE(vblank.met());
    vblank.at_vblank(5);
    CHECK(vblank.met_at() == 5u);

    const auto bind =
        game::trigger_from_event(rt::MachineEvent{"rpc_bind", {}}.add("sid", std::uint64_t{0x80000001}));
    REQUIRE(bind);
    CHECK(*bind == rt::InputTrigger{"rpc_bind", "0x80000001"});
    CHECK_FALSE(game::trigger_from_event(rt::MachineEvent{"thread", {}}));
}
