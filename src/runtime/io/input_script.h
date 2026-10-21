// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/pad_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Scripted input: a `.dq8in` file (Docs/formats/dq8-input-script.md) played back as an IInputSource. Lines
// take effect at absolute vblanks, relative to the previous line (`+N`), or relative to the first occurrence
// of a trigger after the previous line (`after overlay:NAME +N`, `mode:N`, `rpc_bind:SID`). Triggers come
// from the host, which turns machine events into them as they are emitted on the machine thread, so a run
// with the same script, disc and build reads the same pad states.
namespace dq8::rt {

// A moment a script line can wait for. Values are canonical (canonical_trigger_value).
struct InputTrigger {
    std::string kind;  // "overlay", "mode" or "rpc_bind"
    std::string value; // "TITLE", "4", "0x80000001"
    bool operator==(const InputTrigger&) const = default;
};

// The canonical form of a trigger value: overlay names in upper case, modes in decimal, RPC server ids as
// 0x%08X. Numbers may be written in decimal or 0x hex. nullopt for an unknown kind or a malformed value.
std::optional<std::string> canonical_trigger_value(std::string_view kind, std::string_view value);
// "overlay:TITLE" -> {"overlay", "TITLE"}; nullopt when malformed.
std::optional<InputTrigger> parse_trigger(std::string_view text);

// "cross+down" -> pad_cross | pad_down; "none" -> 0; nullopt for an unknown name.
std::optional<std::uint16_t> parse_buttons(std::string_view text);
// pad_cross | pad_down -> "down+cross" (bit order); 0 -> "none".
std::string format_buttons(std::uint16_t buttons);

struct InputScriptLine {
    enum class Time { absolute, relative, after };
    enum class Action { press, hold, release, state, stick, connect, disconnect };

    std::size_t line = 0; // 1-based line number in the file
    Time time = Time::absolute;
    std::uint64_t vblank = 0; // absolute: the vblank; relative and after: the offset
    InputTrigger trigger;     // after
    unsigned port = 0;        // 0 (p1) or 1 (p2)
    Action action = Action::press;
    std::uint16_t buttons = 0;       // press, hold, release (release: all bits for "all"), state
    std::uint32_t duration = 4;      // press: vblanks held
    bool left_stick = true;          // stick
    std::uint8_t x = 0x80, y = 0x80; // stick
    // state: the sticks it sets (absent = centre).
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
};

struct InputScript {
    std::string name; // file name, for messages
    std::vector<InputScriptLine> lines;
};

// Throws std::invalid_argument with "<name>:<line>: <problem>" for the first malformed line, or when the
// `dq8in 1` header is missing.
InputScript parse_input_script(std::string_view text, std::string name = "<script>");
InputScript load_input_script(const std::filesystem::path& file);

class ScriptInput final : public IInputSource {
  public:
    explicit ScriptInput(InputScript script);

    // A trigger happened when `vblank` vblanks had started (MachineServices::vblank_count at emission).
    void note_trigger(const InputTrigger& trigger, std::uint64_t vblank);

    PadState sample(unsigned port, std::uint64_t vblank) override;

    // Lines applied so far, and the first line still waiting for its time or trigger (nullopt when done).
    std::size_t applied() const noexcept { return next_; }
    std::optional<std::size_t> waiting_line() const;
    bool finished() const noexcept { return next_ == script_.lines.size(); }

  private:
    struct Press {
        std::uint16_t buttons = 0;
        std::uint64_t until = 0; // first vblank no longer pressed
    };
    struct Port {
        bool connected = true;
        std::uint16_t held = 0;
        std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
        std::vector<Press> presses;
    };
    // Applies every line whose time is known and <= vblank.
    void advance(std::uint64_t vblank);
    std::optional<std::uint64_t> resolve_time(const InputScriptLine& line) const;
    void apply(const InputScriptLine& line, std::uint64_t vblank);

    InputScript script_;
    std::size_t next_ = 0;
    std::uint64_t previous_time_ = 0; // resolved time of the last applied line
    std::vector<std::pair<InputTrigger, std::uint64_t>> history_;
    std::array<Port, 2> ports_{};
    std::optional<std::uint64_t> advanced_; // last vblank advance() ran for
};

// The PadState a port reads: buttons, sticks, and full pressure for every pressed pressure-sensitive button.
PadState make_pad_state(bool connected, std::uint16_t buttons, std::uint8_t lx = 0x80, std::uint8_t ly = 0x80,
                        std::uint8_t rx = 0x80, std::uint8_t ry = 0x80);

} // namespace dq8::rt
