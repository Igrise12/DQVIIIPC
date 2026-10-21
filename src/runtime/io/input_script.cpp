// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/input_script.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace dq8::rt {

namespace {

struct ButtonName {
    std::string_view name;
    std::uint16_t bit;
};

// In PadButton bit order, which is also the order format_buttons() writes.
constexpr ButtonName button_names[] = {
    {"select", pad_select},
    {"l3", pad_l3},
    {"r3", pad_r3},
    {"start", pad_start},
    {"up", pad_up},
    {"right", pad_right},
    {"down", pad_down},
    {"left", pad_left},
    {"l2", pad_l2},
    {"r2", pad_r2},
    {"l1", pad_l1},
    {"r1", pad_r1},
    {"triangle", pad_triangle},
    {"circle", pad_circle},
    {"cross", pad_cross},
    {"square", pad_square},
};

// PadState::pressure order: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2.
constexpr std::uint16_t pressure_order[12] = {pad_right,    pad_left,   pad_up,    pad_down,
                                              pad_triangle, pad_circle, pad_cross, pad_square,
                                              pad_l1,       pad_r1,     pad_l2,    pad_r2};

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
    return out;
}

std::optional<std::uint64_t> parse_uint(std::string_view text) {
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parse_int(std::string_view text) {
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    const auto v = parse_uint(text);
    if (!v || *v > static_cast<std::uint64_t>(INT64_MAX)) {
        return std::nullopt;
    }
    return negative ? -static_cast<std::int64_t>(*v) : static_cast<std::int64_t>(*v);
}

std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i > start) {
            words.push_back(line.substr(start, i - start));
        }
    }
    return words;
}

std::optional<std::uint8_t> parse_axis(std::string_view text) {
    const auto v = parse_uint(text);
    if (!v || *v > 0xFF) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*v);
}

} // namespace

std::optional<std::string> canonical_trigger_value(std::string_view kind, std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    if (kind == "overlay") {
        for (const char c : value) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                return std::nullopt;
            }
        }
        return upper(value);
    }
    if (kind == "mode") {
        const auto v = parse_int(value);
        if (!v) {
            return std::nullopt;
        }
        return std::to_string(*v);
    }
    if (kind == "mark") {
        // A script's own mark (the `mark NAME` action): letters, digits, '_' and '-', kept as written.
        for (const char c : value) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                return std::nullopt;
            }
        }
        return std::string(value);
    }
    if (kind == "event") {
        // Any machine event: TYPE, or TYPE:FIELD=VALUE (event:movie:phase=end). Kept as written; the host
        // matches it against the events it emits.
        const std::size_t colon = value.find(':');
        const std::string_view type = value.substr(0, colon);
        const auto word = [](std::string_view t) {
            if (t.empty()) {
                return false;
            }
            for (const char c : t) {
                if (!std::islower(static_cast<unsigned char>(c)) &&
                    !std::isdigit(static_cast<unsigned char>(c)) && c != '_') {
                    return false;
                }
            }
            return true;
        };
        if (!word(type)) {
            return std::nullopt;
        }
        if (colon != std::string_view::npos) {
            const std::string_view rest = value.substr(colon + 1);
            const std::size_t eq = rest.find('=');
            if (eq == std::string_view::npos || !word(rest.substr(0, eq)) || eq + 1 >= rest.size()) {
                return std::nullopt;
            }
        }
        return std::string(value);
    }
    if (kind == "rpc_bind") {
        const auto v = parse_uint(value);
        if (!v || *v > 0xFFFFFFFFu) {
            return std::nullopt;
        }
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08X", static_cast<unsigned>(*v));
        return std::string(buf);
    }
    return std::nullopt;
}

std::optional<InputTrigger> parse_trigger(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string kind = lower(text.substr(0, colon));
    const auto value = canonical_trigger_value(kind, text.substr(colon + 1));
    if (!value) {
        return std::nullopt;
    }
    return InputTrigger{kind, *value};
}

std::optional<std::uint16_t> parse_buttons(std::string_view text) {
    const std::string t = lower(text);
    if (t == "none") {
        return std::uint16_t{0};
    }
    std::uint16_t bits = 0;
    std::size_t start = 0;
    while (start <= t.size()) {
        const std::size_t plus = t.find('+', start);
        const std::string_view name =
            std::string_view(t).substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        bool found = false;
        for (const ButtonName& b : button_names) {
            if (b.name == name) {
                bits = static_cast<std::uint16_t>(bits | b.bit);
                found = true;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        if (plus == std::string::npos) {
            break;
        }
        start = plus + 1;
    }
    return bits;
}

std::string format_buttons(std::uint16_t buttons) {
    if (buttons == 0) {
        return "none";
    }
    std::string out;
    for (const ButtonName& b : button_names) {
        if ((buttons & b.bit) != 0) {
            out += (out.empty() ? "" : "+") + std::string(b.name);
        }
    }
    return out;
}

PadState make_pad_state(bool connected, std::uint16_t buttons, std::uint8_t lx, std::uint8_t ly,
                        std::uint8_t rx, std::uint8_t ry) {
    PadState s;
    s.connected = connected;
    s.buttons = connected ? buttons : 0;
    s.lx = lx;
    s.ly = ly;
    s.rx = rx;
    s.ry = ry;
    for (std::size_t i = 0; i < s.pressure.size(); ++i) {
        s.pressure[i] = (s.buttons & pressure_order[i]) != 0 ? 0xFF : 0x00;
    }
    return s;
}

InputScript parse_input_script(std::string_view text, std::string name) {
    InputScript script;
    script.name = std::move(name);
    bool header = false;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++line_no;
        if (const std::size_t hash = raw.find('#'); hash != std::string_view::npos) {
            raw = raw.substr(0, hash);
        }
        const std::vector<std::string_view> w = split_words(raw);
        if (w.empty()) {
            continue;
        }
        const auto fail = [&](const std::string& why) -> std::invalid_argument {
            return std::invalid_argument(script.name + ":" + std::to_string(line_no) + ": " + why);
        };
        if (!header) {
            if (w.size() != 2 || w[0] != "dq8in") {
                throw fail("the first line must be the header 'dq8in 1'");
            }
            if (w[1] != "1" && w[1] != "2") {
                throw fail("unsupported format version '" + std::string(w[1]) +
                           "' (this build reads 1 and 2)");
            }
            header = true;
            continue;
        }

        InputScriptLine l;
        l.line = line_no;
        std::size_t i = 0;
        if (w[0] == "after") {
            if (w.size() < 3) {
                throw fail("'after' needs a trigger and an offset: after overlay:NAME +N ...");
            }
            const auto trigger = parse_trigger(w[1]);
            if (!trigger) {
                throw fail("unknown trigger '" + std::string(w[1]) +
                           "' (overlay:NAME, mode:N, rpc_bind:SID, mark:NAME or event:TYPE[:FIELD=VALUE])");
            }
            if (w[2].size() < 2 || w[2][0] != '+') {
                throw fail("'after " + std::string(w[1]) + "' needs an offset like +0");
            }
            const auto offset = parse_uint(w[2].substr(1));
            if (!offset) {
                throw fail("'" + std::string(w[2]) + "' is not an offset in vblanks");
            }
            l.time = InputScriptLine::Time::after;
            l.trigger = *trigger;
            l.vblank = *offset;
            i = 3;
        } else if (w[0][0] == '+') {
            const auto offset = parse_uint(w[0].substr(1));
            if (!offset) {
                throw fail("'" + std::string(w[0]) + "' is not an offset in vblanks");
            }
            l.time = InputScriptLine::Time::relative;
            l.vblank = *offset;
            i = 1;
        } else {
            const auto at = parse_uint(w[0]);
            if (!at) {
                throw fail("a line starts with a vblank number, +N or 'after', not '" + std::string(w[0]) +
                           "'");
            }
            l.time = InputScriptLine::Time::absolute;
            l.vblank = *at;
            i = 1;
        }
        if (i < w.size() && (w[i] == "p1" || w[i] == "p2")) {
            l.port = w[i] == "p1" ? 0 : 1;
            ++i;
        }
        if (i >= w.size()) {
            throw fail("missing action (press, hold, release, state, stick, connect, disconnect, mark)");
        }
        const std::string_view action = w[i++];
        const std::size_t args = w.size() - i;
        const auto buttons_at = [&](std::size_t k) {
            const auto b = parse_buttons(w[k]);
            if (!b) {
                throw fail("unknown button in '" + std::string(w[k]) + "'");
            }
            return *b;
        };
        if (action == "press") {
            if (args < 1 || args > 2) {
                throw fail("press BUTTONS [VBLANKS]");
            }
            l.action = InputScriptLine::Action::press;
            l.buttons = buttons_at(i);
            if (args == 2) {
                const auto d = parse_uint(w[i + 1]);
                if (!d || *d == 0 || *d > 0xFFFFFFFFu) {
                    throw fail("'" + std::string(w[i + 1]) + "' is not a press length in vblanks (>= 1)");
                }
                l.duration = static_cast<std::uint32_t>(*d);
            }
        } else if (action == "hold") {
            if (args != 1) {
                throw fail("hold BUTTONS");
            }
            l.action = InputScriptLine::Action::hold;
            l.buttons = buttons_at(i);
        } else if (action == "release") {
            if (args != 1) {
                throw fail("release BUTTONS|all");
            }
            l.action = InputScriptLine::Action::release;
            l.buttons = w[i] == "all" ? std::uint16_t{0xFFFF} : buttons_at(i);
        } else if (action == "state") {
            if (args < 1) {
                throw fail("state BUTTONS|none [lx=V] [ly=V] [rx=V] [ry=V]");
            }
            l.action = InputScriptLine::Action::state;
            l.buttons = buttons_at(i);
            for (std::size_t k = i + 1; k < w.size(); ++k) {
                const std::size_t eq = w[k].find('=');
                const std::string_view key = w[k].substr(0, eq);
                const auto v = eq == std::string_view::npos ? std::nullopt : parse_axis(w[k].substr(eq + 1));
                if (!v) {
                    throw fail("'" + std::string(w[k]) + "' is not an axis value like lx=0x80");
                }
                if (key == "lx") {
                    l.lx = *v;
                } else if (key == "ly") {
                    l.ly = *v;
                } else if (key == "rx") {
                    l.rx = *v;
                } else if (key == "ry") {
                    l.ry = *v;
                } else {
                    throw fail("unknown axis '" + std::string(key) + "' (lx, ly, rx, ry)");
                }
            }
        } else if (action == "stick") {
            if (args != 3 || (w[i] != "left" && w[i] != "right")) {
                throw fail("stick left|right X Y");
            }
            l.action = InputScriptLine::Action::stick;
            l.left_stick = w[i] == "left";
            const auto x = parse_axis(w[i + 1]);
            const auto y = parse_axis(w[i + 2]);
            if (!x || !y) {
                throw fail("stick values are 0..255 (0x80 is the centre)");
            }
            l.x = *x;
            l.y = *y;
        } else if (action == "connect" || action == "disconnect") {
            if (args != 0) {
                throw fail(std::string(action) + " takes no arguments");
            }
            l.action =
                action == "connect" ? InputScriptLine::Action::connect : InputScriptLine::Action::disconnect;
        } else if (action == "mark") {
            const auto mark = args == 1 ? canonical_trigger_value("mark", w[i]) : std::nullopt;
            if (!mark) {
                throw fail("mark NAME (letters, digits, '_' and '-')");
            }
            l.action = InputScriptLine::Action::mark;
            l.name = *mark;
        } else {
            throw fail("unknown action '" + std::string(action) +
                       "' (press, hold, release, state, stick, connect, disconnect, mark)");
        }
        script.lines.push_back(l);
    }
    if (!header) {
        throw std::invalid_argument(script.name + ":1: the first line must be the header 'dq8in 1'");
    }
    return script;
}

InputScript load_input_script(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::invalid_argument("cannot open input script " + file.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parse_input_script(text.str(), file.filename().string());
}

ScriptInput::ScriptInput(InputScript script) : script_(std::move(script)) {}

void ScriptInput::note_trigger(const InputTrigger& trigger, std::uint64_t vblank) {
    history_.emplace_back(trigger, vblank);
}

std::optional<std::size_t> ScriptInput::waiting_line() const {
    if (finished()) {
        return std::nullopt;
    }
    return script_.lines[next_].line;
}

std::optional<std::uint64_t> ScriptInput::resolve_time(const InputScriptLine& line) const {
    switch (line.time) {
    case InputScriptLine::Time::absolute:
        return std::max(line.vblank, previous_time_);
    case InputScriptLine::Time::relative:
        return previous_time_ + line.vblank;
    case InputScriptLine::Time::after:
        for (const auto& [trigger, at] : history_) {
            if (trigger == line.trigger && at >= previous_time_) {
                return at + line.vblank;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void ScriptInput::apply(const InputScriptLine& line, std::uint64_t vblank) {
    Port& p = ports_[line.port];
    switch (line.action) {
    case InputScriptLine::Action::press:
        p.presses.push_back({line.buttons, vblank + line.duration});
        break;
    case InputScriptLine::Action::hold:
        p.held = static_cast<std::uint16_t>(p.held | line.buttons);
        break;
    case InputScriptLine::Action::release:
        p.held = static_cast<std::uint16_t>(p.held & ~line.buttons);
        if (line.buttons == 0xFFFF) {
            p.presses.clear();
        }
        break;
    case InputScriptLine::Action::state:
        p.held = line.buttons;
        p.presses.clear();
        p.lx = line.lx;
        p.ly = line.ly;
        p.rx = line.rx;
        p.ry = line.ry;
        break;
    case InputScriptLine::Action::stick:
        (line.left_stick ? p.lx : p.rx) = line.x;
        (line.left_stick ? p.ly : p.ry) = line.y;
        break;
    case InputScriptLine::Action::connect:
        p.connected = true;
        break;
    case InputScriptLine::Action::disconnect:
        p.connected = false;
        break;
    case InputScriptLine::Action::mark:
        if (mark_sink_) {
            mark_sink_(line.name, vblank);
        }
        break;
    }
}

void ScriptInput::advance(std::uint64_t vblank) {
    while (next_ < script_.lines.size()) {
        const InputScriptLine& line = script_.lines[next_];
        const auto at = resolve_time(line);
        if (!at || *at > vblank) {
            return;
        }
        // A line reached late (its trigger came after its time) takes effect now, for its full length.
        apply(line, vblank);
        previous_time_ = *at;
        ++next_;
    }
}

PadState ScriptInput::sample(unsigned port, std::uint64_t vblank) {
    if (!advanced_ || *advanced_ != vblank) {
        advance(vblank);
        advanced_ = vblank;
    }
    Port& p = ports_[port & 1u];
    std::uint16_t buttons = p.held;
    for (const Press& press : p.presses) {
        if (vblank < press.until) {
            buttons = static_cast<std::uint16_t>(buttons | press.buttons);
        }
    }
    std::erase_if(p.presses, [vblank](const Press& press) { return press.until <= vblank; });
    return make_pad_state(p.connected, buttons, p.lx, p.ly, p.rx, p.ry);
}

} // namespace dq8::rt
