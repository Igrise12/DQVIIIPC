// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/clock.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Deterministic timed events on the virtual clock: vblank start and end, timer compares, DMA, SIF, RPC and
// disc completions, alarms. Events run on the machine thread from the scheduler loop, never from inside guest
// code, in (time, scheduling order) order, so two runs see the same sequence.
namespace dq8::rt {

using EventId = std::uint64_t;

class EventQueue {
  public:
    // `when` is the event's scheduled time (not the clock's current value).
    using Callback = std::function<void(Cycles when)>;

    struct PendingEvent {
        EventId id = 0;
        Cycles at = 0;
        std::string_view tag;
    };

    // Schedules `callback` at `at`. `tag` names the event in reports and must outlive it (a string literal).
    // Events at the same time run in the order they were scheduled. Returns a non-zero id. The schedule hook,
    // when set, is told `at` afterwards.
    EventId schedule(Cycles at, std::string_view tag, Callback callback);

    // Called with the time of every newly scheduled event. The machine uses it to pull the clock's deadline
    // forward, so that an event a device schedules in the middle of a slice (an RPC reply, a DMA completion)
    // runs at the next safe point after its time instead of at the next deadline set before it existed.
    using ScheduleHook = std::function<void(Cycles at)>;
    void set_schedule_hook(ScheduleHook hook) { hook_ = std::move(hook); }
    // Removes a pending event; returns false when it already ran or was cancelled.
    bool cancel(EventId id);

    // Runs every event with at <= now, including ones the callbacks schedule at or before `now`. Returns how
    // many ran.
    std::size_t run_due(Cycles now);

    std::optional<Cycles> next_time() const noexcept;
    bool empty() const noexcept { return events_.empty(); }
    std::size_t size() const noexcept { return events_.size(); }
    // The pending events in run order, for deadlock and no-progress reports.
    std::vector<PendingEvent> pending() const;
    void clear() noexcept;

  private:
    struct Entry {
        EventId id = 0;
        std::string_view tag;
        Callback callback;
    };
    using Key = std::pair<Cycles, std::uint64_t>; // (time, sequence)

    std::map<Key, Entry> events_;
    std::unordered_map<EventId, Key> index_;
    std::uint64_t sequence_ = 0;
    EventId next_id_ = 1;
    ScheduleHook hook_;
};

} // namespace dq8::rt
