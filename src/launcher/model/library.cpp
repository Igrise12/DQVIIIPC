// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/library.h"

#include <system_error>
#include <utility>

namespace dq8::launcher {

LibraryModel::LibraryModel(std::unique_ptr<LibraryBackend> backend, std::string host_version)
    : backend_(std::move(backend)), host_version_(std::move(host_version)) {}

void LibraryModel::refresh() {
    std::string previous;
    if (const LibraryEntry* e = current()) {
        previous = e->install.profile_id;
    }
    entries_.clear();
    problem_.reset();
    std::vector<core::registry::Install> installs;
    try {
        installs = backend_->installs();
    } catch (...) {
        problem_ = explain_current_exception();
    }
    for (core::registry::Install& install : installs) {
        LibraryEntry e;
        std::error_code ec;
        e.image_found = std::filesystem::is_regular_file(install.iso, ec);
        e.modules_found = std::filesystem::is_directory(install.module_dir, ec);
        e.other_version = !install.host_version.empty() && install.host_version != host_version_;
        try {
            e.title = backend_->title(install.profile_id);
        } catch (...) {
            // the title is decoration; the profile id stands in
        }
        e.install = std::move(install);
        entries_.push_back(std::move(e));
    }
    selected_ = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].install.profile_id == previous) {
            selected_ = i;
        }
    }
}

void LibraryModel::select(std::size_t index) noexcept {
    if (index < entries_.size()) {
        selected_ = index;
    }
}

const LibraryEntry* LibraryModel::current() const noexcept {
    return selected_ < entries_.size() ? &entries_[selected_] : nullptr;
}

std::optional<PlayRequest> LibraryModel::play_request(const std::filesystem::path& game,
                                                      const std::filesystem::path& config_file) const {
    const LibraryEntry* e = current();
    if (e == nullptr || !e->image_found || !e->modules_found || game.empty()) {
        return std::nullopt;
    }
    PlayRequest r;
    r.game = game;
    r.iso = e->install.iso;
    r.module_dir = e->install.module_dir;
    r.config_file = config_file;
    r.profile_id = e->install.profile_id;
    return r;
}

} // namespace dq8::launcher
