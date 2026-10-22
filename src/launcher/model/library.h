// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "dq8/core/registry.h"
#include "model/errors.h"
#include "model/game_session.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Play page's list: the builds the setup registered (core::registry, installs.toml), with what the page
// needs to say before Play is pressed (the disc image or the modules have gone missing, another program
// version built them).
namespace dq8::launcher {

class LibraryBackend {
  public:
    virtual ~LibraryBackend() = default;
    virtual std::vector<core::registry::Install> installs() = 0; // throws core::Error
    // The region profile's title for people, or empty.
    virtual std::string title(std::string_view profile_id) = 0;
};

// core::registry over registry::default_file(), titles from the region profiles found next to the program.
std::unique_ptr<LibraryBackend> registry_library(const std::filesystem::path& executable_dir);

struct LibraryEntry {
    core::registry::Install install;
    std::string title;
    bool image_found = false;
    bool modules_found = false;
    bool other_version = false; // built by a different program version; a rebuild is advised
};

class LibraryModel {
  public:
    LibraryModel(std::unique_ptr<LibraryBackend> backend, std::string host_version);

    void refresh();
    const std::vector<LibraryEntry>& entries() const noexcept { return entries_; }
    const std::optional<Problem>& problem() const noexcept { return problem_; }
    bool empty() const noexcept { return entries_.empty(); }

    std::size_t selected() const noexcept { return selected_; }
    void select(std::size_t index) noexcept;
    const LibraryEntry* current() const noexcept;

    // What Play starts for the selected entry; nullopt when there is none or its files are missing.
    std::optional<PlayRequest> play_request(const std::filesystem::path& game,
                                            const std::filesystem::path& config_file) const;

  private:
    std::unique_ptr<LibraryBackend> backend_;
    std::string host_version_;
    std::vector<LibraryEntry> entries_;
    std::optional<Problem> problem_;
    std::size_t selected_ = 0;
};

} // namespace dq8::launcher
