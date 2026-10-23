// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/log_buffer.h"

namespace dq8::launcher {

void LogBuffer::add(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    for (;;) {
        const std::size_t nl = line.find('\n');
        std::string_view piece = line.substr(0, nl);
        if (!piece.empty() && piece.back() == '\r') {
            piece.remove_suffix(1);
        }
        lines_.emplace_back(piece);
        if (lines_.size() > capacity_) {
            lines_.pop_front();
            ++dropped_;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        line.remove_prefix(nl + 1);
    }
    ++generation_;
}

void LogBuffer::clear() {
    lines_.clear();
    dropped_ = 0;
    ++generation_;
}

std::string LogBuffer::text() const {
    std::string out;
    for (const std::string& l : lines_) {
        out += l;
        out += '\n';
    }
    return out;
}

} // namespace dq8::launcher
