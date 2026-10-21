// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Input sources: the .dq8in parser (Docs/formats/dq8-input-script.md), scripted playback with absolute,
// relative and trigger-relative lines, the live latch and the recorder's round trip.
#include "runtime/io/input_live.h"
#include "runtime/io/input_record.h"
#include "runtime/io/input_script.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace dq8::rt;

std::uint16_t buttons_at(ScriptInput& in, unsigned port, std::uint64_t vblank) {
    return in.sample(port, vblank).buttons;
}

std::string parse_error(std::string_view text) {
    try {
        parse_input_script(text, "t.dq8in");
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "<parsed>";
}

} // namespace

TEST_CASE("input script: buttons and triggers", "[input]") {
    CHECK(parse_buttons("cross") == pad_cross);
    CHECK(parse_buttons("Down+CROSS") == (pad_down | pad_cross));
    CHECK(parse_buttons("none") == 0);
    CHECK_FALSE(parse_buttons("cross+jump"));
    CHECK_FALSE(parse_buttons("cross+"));
    CHECK(format_buttons(pad_cross | pad_down) == "down+cross");
    CHECK(format_buttons(0) == "none");
    for (std::uint32_t bit = 1; bit <= 0x8000; bit <<= 1) {
        CHECK(parse_buttons(format_buttons(static_cast<std::uint16_t>(bit))) == bit);
    }

    CHECK(*parse_trigger("overlay:title") == InputTrigger{"overlay", "TITLE"});
    CHECK(*parse_trigger("mode:0x0A") == InputTrigger{"mode", "10"});
    CHECK(*parse_trigger("mode:-1") == InputTrigger{"mode", "-1"});
    CHECK(*parse_trigger("rpc_bind:2147483649") == InputTrigger{"rpc_bind", "0x80000001"});
    CHECK_FALSE(parse_trigger("overlay:"));
    CHECK_FALSE(parse_trigger("overlay:a/b"));
    CHECK_FALSE(parse_trigger("mode:four"));
    CHECK_FALSE(parse_trigger("vsync:3"));
    CHECK_FALSE(parse_trigger("rpc_bind:0x100000000"));
}

TEST_CASE("input script: the parser reports the first bad line", "[input]") {
    CHECK(parse_error("").find("t.dq8in:1: the first line must be the header") != std::string::npos);
    CHECK(parse_error("# only a comment\n10 press cross\n").find("t.dq8in:2:") != std::string::npos);
    CHECK(parse_error("dq8in 2\n").find("unsupported format version '2'") != std::string::npos);
    CHECK(parse_error("dq8in 1\n10 jump\n").find("t.dq8in:2: unknown action 'jump'") != std::string::npos);
    CHECK(parse_error("dq8in 1\nsoon press cross\n").find("not 'soon'") != std::string::npos);
    CHECK(parse_error("dq8in 1\n10 press kick\n").find("unknown button") != std::string::npos);
    CHECK(parse_error("dq8in 1\n10 press cross 0\n").find("press length") != std::string::npos);
    CHECK(parse_error("dq8in 1\nafter overlay:TITLE press cross\n").find("needs an offset") !=
          std::string::npos);
    CHECK(parse_error("dq8in 1\nafter vsync:1 +0 press cross\n").find("unknown trigger") !=
          std::string::npos);
    CHECK(parse_error("dq8in 1\n+x press cross\n").find("not an offset") != std::string::npos);
    CHECK(parse_error("dq8in 1\n10 state cross lz=1\n").find("unknown axis") != std::string::npos);
    CHECK(parse_error("dq8in 1\n10 stick up 1 2\n").find("stick left|right X Y") != std::string::npos);
    CHECK(parse_error("dq8in 1\n10 p3 press cross\n").find("unknown action 'p3'") != std::string::npos);

    const InputScript s = parse_input_script("dq8in 1\n"
                                             "  # comment\n"
                                             "100 press cross   # trailing comment\n"
                                             "+20 p2 hold r1\n"
                                             "after mode:4 +6 release all\n"
                                             "7 state down lx=0x00 ry=255\n"
                                             "+1 stick right 1 2\n"
                                             "+1 disconnect\n",
                                             "s.dq8in");
    REQUIRE(s.lines.size() == 6);
    CHECK(s.lines[0].line == 3);
    CHECK(s.lines[0].duration == 4);
    CHECK(s.lines[1].port == 1);
    CHECK(s.lines[1].time == InputScriptLine::Time::relative);
    CHECK(s.lines[2].trigger == InputTrigger{"mode", "4"});
    CHECK(s.lines[2].buttons == 0xFFFF);
    CHECK(s.lines[3].lx == 0x00);
    CHECK(s.lines[3].ry == 0xFF);
    CHECK(s.lines[4].left_stick == false);
    CHECK(s.lines[5].action == InputScriptLine::Action::disconnect);
}

TEST_CASE("input script: absolute and relative lines", "[input]") {
    ScriptInput in(parse_input_script("dq8in 1\n"
                                      "10 press cross 3\n"
                                      "+5 press down\n" // at 15, 4 vblanks
                                      "+0 p2 hold start\n"
                                      "30 release start\n" // port 1: no effect on port 2's hold
                                      "31 p2 release start\n"));
    std::vector<std::uint16_t> p1;
    for (std::uint64_t v = 0; v < 22; ++v) {
        p1.push_back(buttons_at(in, 0, v));
        buttons_at(in, 1, v);
    }
    CHECK(p1[9] == 0);
    CHECK(p1[10] == pad_cross);
    CHECK(p1[12] == pad_cross);
    CHECK(p1[13] == 0);
    CHECK(p1[15] == pad_down);
    CHECK(p1[18] == pad_down);
    CHECK(p1[19] == 0);
    CHECK(buttons_at(in, 1, 25) == pad_start);
    CHECK(buttons_at(in, 1, 30) == pad_start);
    CHECK(buttons_at(in, 1, 31) == 0);
    CHECK(in.finished());

    const PadState s = make_pad_state(true, pad_cross | pad_r2);
    CHECK(s.pressure[6] == 0xFF);  // cross
    CHECK(s.pressure[11] == 0xFF); // R2
    CHECK(s.pressure[0] == 0x00);
}

TEST_CASE("input script: trigger lines wait for the first occurrence after the previous line", "[input]") {
    ScriptInput in(parse_input_script("dq8in 1\n"
                                      "after overlay:TITLE +10 press down 2\n"
                                      "+5 press cross 2\n"
                                      "after mode:4 +0 press circle 1\n"));
    for (std::uint64_t v = 0; v < 50; ++v) {
        CHECK(buttons_at(in, 0, v) == 0);
    }
    CHECK(in.waiting_line() == 2u);
    in.note_trigger({"mode", "4"}, 40); // before the TITLE line resolves: does not count for line 4
    in.note_trigger({"overlay", "TITLE"}, 50);
    CHECK(buttons_at(in, 0, 59) == 0);
    CHECK(buttons_at(in, 0, 60) == pad_down);
    CHECK(buttons_at(in, 0, 61) == pad_down);
    CHECK(buttons_at(in, 0, 62) == 0);
    CHECK(buttons_at(in, 0, 65) == pad_cross);
    CHECK(in.waiting_line() == 4u);
    CHECK(buttons_at(in, 0, 70) == 0);
    in.note_trigger({"mode", "4"}, 71);
    CHECK(buttons_at(in, 0, 71) == pad_circle);
    CHECK(buttons_at(in, 0, 72) == 0);
    CHECK(in.finished());
}

TEST_CASE("input script: a late line applies at the next sample for its full length", "[input]") {
    ScriptInput in(parse_input_script("dq8in 1\nafter rpc_bind:0x80000100 +0 press triangle 3\n"));
    buttons_at(in, 0, 9);
    in.note_trigger({"rpc_bind", "0x80000100"}, 9); // emitted after vblank 9 was sampled
    CHECK(buttons_at(in, 0, 10) == pad_triangle);
    CHECK(buttons_at(in, 0, 12) == pad_triangle);
    CHECK(buttons_at(in, 0, 13) == 0);
}

TEST_CASE("input script: state, sticks and connection", "[input]") {
    ScriptInput in(parse_input_script("dq8in 1\n"
                                      "5 state cross+up lx=0x10\n"
                                      "6 stick right 0x20 0x30\n"
                                      "7 disconnect\n"
                                      "8 connect\n"
                                      "9 state none\n"));
    CHECK(in.sample(0, 5).buttons == (pad_cross | pad_up));
    CHECK(in.sample(0, 5).lx == 0x10);
    const PadState s6 = in.sample(0, 6);
    CHECK(s6.rx == 0x20);
    CHECK(s6.ry == 0x30);
    CHECK(s6.lx == 0x10);
    CHECK_FALSE(in.sample(0, 7).connected);
    CHECK(in.sample(0, 7).buttons == 0);
    CHECK(in.sample(0, 8).buttons == (pad_cross | pad_up));
    const PadState s9 = in.sample(0, 9);
    CHECK(s9.buttons == 0);
    CHECK(s9.lx == 0x80);
    CHECK(s9.rx == 0x80);
}

TEST_CASE("live input: both ports are latched once per vblank", "[input]") {
    LiveInput live;
    live.publish(0, make_pad_state(true, pad_cross));
    CHECK(live.sample(0, 1).buttons == pad_cross);
    live.publish(0, make_pad_state(true, pad_circle));
    live.publish(1, make_pad_state(true, pad_start));
    CHECK(live.sample(0, 1).buttons == pad_cross); // same vblank: the latch holds
    CHECK(live.sample(1, 1).buttons == 0);
    CHECK(live.sample(1, 2).buttons == pad_start);
    CHECK(live.sample(0, 2).buttons == pad_circle);
    IdleInput idle;
    CHECK(idle.sample(1, 9).connected);
    CHECK(idle.sample(1, 9).buttons == 0);
}

TEST_CASE("record input: the recording replays the same states", "[input]") {
    const std::string source = "dq8in 1\n"
                               "3 press cross 2\n"
                               "+4 p2 state down lx=0x00\n"
                               "12 disconnect\n"
                               "14 connect\n"
                               "20 hold square\n";
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() /
        ("dq8_record_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".dq8in");
    std::vector<PadState> original;
    {
        ScriptInput script(parse_input_script(source));
        RecordingInput recorder(script, file);
        for (std::uint64_t v = 0; v < 25; ++v) {
            original.push_back(recorder.sample(0, v));
            original.push_back(recorder.sample(1, v));
        }
        CHECK(recorder.lines() > 0);
    }
    ScriptInput replay(load_input_script(file));
    for (std::uint64_t v = 0; v < 25; ++v) {
        const PadState a = replay.sample(0, v);
        const PadState b = replay.sample(1, v);
        INFO("vblank " << v);
        CHECK(a.buttons == original[2 * v].buttons);
        CHECK(a.connected == original[2 * v].connected);
        CHECK(b.buttons == original[2 * v + 1].buttons);
        CHECK(b.lx == original[2 * v + 1].lx);
    }
    std::filesystem::remove(file);

    const auto lines =
        RecordingInput::format_changes(1, 7, make_pad_state(true, pad_down, 0x00), std::nullopt);
    CHECK(lines == std::vector<std::string>{"7 p2 state down lx=0x00"});
    CHECK(RecordingInput::format_changes(0, 1, PadState{}, std::nullopt).empty());
    CHECK(RecordingInput::format_changes(0, 2, make_pad_state(false, 0), PadState{}) ==
          std::vector<std::string>{"2 disconnect"});
}
