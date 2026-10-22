// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The build registry (architecture.md §3 stage 7): which games are set up and where their modules are, so the
// launcher can offer Play. One TOML file, installs.toml in platform::user_data_dir(): schema_version, then
// one
// [[install]] table per profile with the fields below (profile, iso, module_dir, manifest_sha1, built_at,
// host_version); keys a later version adds are ignored. Errors throw Error(registry_parse) with
// "<file>:<line>:" or Error(io_*).
namespace dq8::core::registry {

struct Install {
    std::string profile_id;    // "SLUS_21207"
    std::filesystem::path iso; // the image the build came from
    std::filesystem::path module_dir;
    std::string manifest_sha1; // of the module directory's build-manifest.toml
    std::string built_at;      // ISO 8601, UTC
    std::string host_version;  // the program version that built it
};

class Registry {
  public:
    // A missing file gives an empty registry.
    static Registry load(const std::filesystem::path& file);
    // Writes atomically (a temporary file, then a rename).
    void save(const std::filesystem::path& file) const;

    const std::vector<Install>& installs() const noexcept { return installs_; }
    const Install* find(std::string_view profile_id) const noexcept;
    // Adds `install`, or replaces the entry with the same profile id.
    void upsert(Install install);

  private:
    std::vector<Install> installs_;
};

std::filesystem::path default_file(); // platform::user_data_dir() / "installs.toml"

} // namespace dq8::core::registry
