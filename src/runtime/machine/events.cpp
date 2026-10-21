// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/machine/events.h"

namespace dq8::rt {

EventId EventQueue::schedule(Cycles at, std::string_view tag, Callback callback) {
    const EventId id = next_id_++;
    const Key key{at, sequence_++};
    events_.emplace(key, Entry{id, tag, std::move(callback)});
    index_.emplace(id, key);
    return id;
}

bool EventQueue::cancel(EventId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    events_.erase(it->second);
    index_.erase(it);
    return true;
}

std::size_t EventQueue::run_due(Cycles now) {
    std::size_t ran = 0;
    while (!events_.empty() && events_.begin()->first.first <= now) {
        auto node = events_.extract(events_.begin());
        index_.erase(node.mapped().id);
        if (node.mapped().callback) {
            node.mapped().callback(node.key().first);
        }
        ++ran;
    }
    return ran;
}

std::optional<Cycles> EventQueue::next_time() const noexcept {
    if (events_.empty()) {
        return std::nullopt;
    }
    return events_.begin()->first.first;
}

std::vector<EventQueue::PendingEvent> EventQueue::pending() const {
    std::vector<PendingEvent> out;
    out.reserve(events_.size());
    for (const auto& [key, entry] : events_) {
        out.push_back(PendingEvent{entry.id, key.first, entry.tag});
    }
    return out;
}

void EventQueue::clear() noexcept {
    events_.clear();
    index_.clear();
}

} // namespace dq8::rt
