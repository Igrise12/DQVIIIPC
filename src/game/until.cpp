// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "game/until.h"

#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <variant>

namespace dq8::game {

namespace {

const std::string* string_field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k == key) {
            return std::get_if<std::string>(&v);
        }
    }
    return nullptr;
}

// A numeric field as text the trigger parser accepts (integers in decimal, strings as they are).
std::optional<std::string> number_field(const rt::MachineEvent& e, std::string_view key) {
    for (const auto& [k, v] : e.fields) {
        if (k != key) {
            continue;
        }
        if (const auto* i = std::get_if<std::int64_t>(&v)) {
            return std::to_string(*i);
        }
        if (const auto* u = std::get_if<std::uint64_t>(&v)) {
            return std::to_string(*u);
        }
        if (const auto* s = std::get_if<std::string>(&v)) {
            return *s;
        }
    }
    return std::nullopt;
}

} // namespace

UntilCondition parse_until(std::string_view text, const core::RegionProfile* profile) {
    UntilCondition c;
    c.text = std::string(text);
    const std::string usage = "--until " + c.text + ": expected vblank:N, overlay:NAME, mode:N or title";
    if (text == "title") {
        // The profile's [title] marker says what the title screen is; TitleWatcher emits `title` there.
        if (profile == nullptr || profile->title_screen.variable == 0) {
            throw std::invalid_argument("--until title needs the region profile's [title] marker" +
                                        std::string(profile != nullptr ? " (" + profile->id + " has none)"
                                                                       : " (none in developer mode)") +
                                        "; use overlay:NAME, mode:N or vblank:N");
        }
        c.kind = UntilCondition::Kind::title;
        return c;
    }
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument(usage);
    }
    const std::string_view kind = text.substr(0, colon);
    const std::string_view value = text.substr(colon + 1);
    if (kind == "vblank") {
        std::uint64_t n = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (value.empty() || ec != std::errc() || end != value.data() + value.size()) {
            throw std::invalid_argument(usage);
        }
        c.kind = UntilCondition::Kind::vblank;
        c.vblank = n;
        return c;
    }
    if (kind != "overlay" && kind != "mode") {
        throw std::invalid_argument(usage);
    }
    const auto trigger = rt::parse_trigger(text);
    if (!trigger) {
        throw std::invalid_argument(usage);
    }
    if (kind == "overlay" && profile != nullptr) {
        bool known = false;
        std::string names;
        for (const core::ProfileOverlay& o : profile->overlays) {
            known = known || o.name == trigger->value;
            names += (names.empty() ? "" : ", ") + o.name;
        }
        if (!known) {
            throw std::invalid_argument("--until " + c.text + ": " + profile->id + " has no overlay " +
                                        trigger->value + " (" + names + ")");
        }
    }
    c.kind = UntilCondition::Kind::trigger;
    c.trigger = *trigger;
    return c;
}

std::optional<rt::InputTrigger> trigger_from_event(const rt::MachineEvent& event) {
    std::string kind;
    std::optional<std::string> value;
    if (event.type == "overlay_resident") {
        kind = "overlay";
        if (const std::string* name = string_field(event, "name")) {
            value = *name;
        }
    } else if (event.type == "mode") {
        kind = "mode";
        value = number_field(event, "mode");
    } else if (event.type == "rpc_bind") {
        kind = "rpc_bind";
        value = number_field(event, "sid");
    } else {
        return std::nullopt;
    }
    if (!value) {
        return std::nullopt;
    }
    const auto canonical = rt::canonical_trigger_value(kind, *value);
    if (!canonical) {
        return std::nullopt;
    }
    return rt::InputTrigger{kind, *canonical};
}

void UntilTracker::observe(const rt::MachineEvent& event, std::uint64_t vblank) {
    if (met_at_) {
        return;
    }
    if (condition_.kind == UntilCondition::Kind::title) {
        if (event.type == "title") {
            met_at_ = vblank;
        }
        return;
    }
    if (condition_.kind != UntilCondition::Kind::trigger) {
        return;
    }
    if (const auto t = trigger_from_event(event); t && *t == condition_.trigger) {
        met_at_ = vblank;
    }
}

void UntilTracker::at_vblank(std::uint64_t vblank) {
    if (!met_at_ && condition_.kind == UntilCondition::Kind::vblank && vblank >= condition_.vblank) {
        met_at_ = vblank;
    }
}

} // namespace dq8::game
