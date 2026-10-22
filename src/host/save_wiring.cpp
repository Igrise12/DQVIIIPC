// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M4 session D: the memory cards (--save-dir, --memory-card, --memory-card2) and the virtual RTC (--rtc),
// ADR-0016. Each port holds an in-memory card (headless by default, or --save-dir none) or a folder card at
// <save-dir>/<profile>/card<N>/ (the window's default save directory is the user data directory); the
// folder's writes finish when the machine has stopped. The end event carries each card's digest.
#include "core/platform/paths.h"
#include "host/m4_wiring.h"
#include "runtime/io/memcard/memory_card.h"
#include "runtime/io/memcard/memory_card_store.h"
#include "runtime/machine/rtc.h"

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dq8::host {

namespace {

// Owns the RTC and the cards for as long as the IOP that points at them (M4Wiring::devices outlive it).
class SaveDevice final : public rt::IDevice {
  public:
    explicit SaveDevice(const rt::RtcDateTime& epoch) : rtc(epoch) {}
    std::string_view name() const override { return "saves"; }
    void attach(rt::MachineServices& /*machine*/) override {}
    void reset() override {}

    rt::VirtualRtc rtc;
    std::array<std::unique_ptr<rt::IMemoryCardStore>, 2> cards;
};

rt::RtcDateTime rtc_epoch(const Options& o) {
    if (!o.rtc) {
        return rt::VirtualRtc::default_epoch();
    }
    if (*o.rtc == "host") {
        return rt::VirtualRtc::host_now();
    }
    if (const auto t = rt::parse_rtc(*o.rtc)) {
        return *t;
    }
    throw UsageError("--rtc '" + *o.rtc + "': not a date that exists");
}

// The directory that holds <profile>/card<N>, or nullopt for in-memory cards.
std::optional<std::filesystem::path> save_dir(const Options& o) {
    if (o.save_dir) {
        if (*o.save_dir == "none") {
            return std::nullopt;
        }
        return std::filesystem::path(*o.save_dir);
    }
    if (o.display != Display::window) {
        return std::nullopt; // scripted runs never touch user files
    }
    try {
        return core::platform::user_data_dir();
    } catch (const std::exception& e) {
        throw UsageError(std::string("the default save directory: ") + e.what() + "; give --save-dir");
    }
}

} // namespace

void wire_saves(M4Wiring& w) {
    const Options& o = w.options;
    auto saves = std::make_unique<SaveDevice>(rtc_epoch(o));
    const std::optional<std::filesystem::path> dir = save_dir(o);
    const std::string profile = w.profile != nullptr ? w.profile->id : std::string("dev");
    const std::array<const std::string*, 2> states{&o.memory_card, &o.memory_card2};
    for (std::size_t port = 0; port < states.size(); ++port) {
        if (*states[port] == "none") {
            continue;
        }
        const bool formatted = *states[port] == "formatted";
        if (!dir) {
            saves->cards[port] = rt::make_memory_card_store(formatted, saves->rtc.epoch());
            continue;
        }
        const std::filesystem::path folder = *dir / profile / ("card" + std::to_string(port + 1));
        try {
            saves->cards[port] = rt::open_folder_card_store(folder, formatted, saves->rtc.epoch());
        } catch (const std::exception& e) {
            throw UsageError(std::string("--save-dir: ") + e.what());
        }
    }

    for (std::size_t port = 0; port < saves->cards.size(); ++port) {
        w.iop.cards[port] = saves->cards[port].get();
    }
    w.iop.rtc = &saves->rtc;

    SaveDevice* s = saves.get();
    w.on_stop.push_back([s] {
        for (const auto& card : s->cards) {
            if (card) {
                card->finish();
                if (card->write_errors() != 0) {
                    std::fprintf(
                        stderr, "memory card %s: %llu writes failed; the folder may be behind the game\n",
                        card->describe().c_str(), static_cast<unsigned long long>(card->write_errors()));
                }
            }
        }
    });
    w.end_fields.push_back([s](rt::MachineEvent& end) {
        if (s->cards[0]) {
            end.add("card_sha1", s->cards[0]->card().sha1());
        }
        if (s->cards[1]) {
            end.add("card2_sha1", s->cards[1]->card().sha1());
        }
    });
    w.devices.push_back(std::move(saves));
}

} // namespace dq8::host
