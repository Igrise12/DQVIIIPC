// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/core_backends.h"

#include "dq8/core/error.h"
#include "dq8/core/intake.h"
#include "dq8/core/region_profile.h"
#include "dq8/core/registry.h"
#include "model/library.h"

#include <map>
#include <system_error>
#include <utility>
#include <vector>

namespace dq8::launcher {

namespace fs = std::filesystem;

namespace {

std::vector<core::RegionProfile> load_profiles(const fs::path& executable_dir) {
    const std::optional<fs::path> dir = find_regions_dir(executable_dir);
    if (!dir) {
        throw core::Error(core::ErrorCode::io_open,
                          "no region profiles (data/regions) were found next to the launcher");
    }
    return core::load_region_profiles(*dir);
}

class CoreWizardBackend final : public WizardBackend {
  public:
    explicit CoreWizardBackend(fs::path executable_dir) : executable_dir_(std::move(executable_dir)) {}

    VerifySummary verify(const fs::path& iso) override {
        const std::vector<core::RegionProfile> profiles = load_profiles(executable_dir_);
        const core::VerifiedImage v = core::verify_image(iso, profiles);
        VerifySummary s;
        s.profile_id = v.profile.id;
        s.title = v.profile.title;
        s.boot_sha1 = v.boot.sha1;
        s.image_size = v.image_size;
        s.warnings = v.warnings;
        return s;
    }

    ToolchainReport check_toolchain() override {
        core::toolchain::DetectOptions options;
        options.executable_dir = executable_dir_;
        ToolchainReport report;
        report.toolchain = core::toolchain::detect(options);
        report.problems = core::toolchain::check(report.toolchain);
        return report;
    }

    unsigned default_jobs() override { return core::toolchain::default_jobs(); }

    core::setup::Result run_setup(const core::setup::Options& options) override {
        return core::setup::run(options);
    }

  private:
    fs::path executable_dir_;
};

class RegistryLibrary final : public LibraryBackend {
  public:
    explicit RegistryLibrary(fs::path executable_dir) : executable_dir_(std::move(executable_dir)) {}

    std::vector<core::registry::Install> installs() override {
        return core::registry::Registry::load(core::registry::default_file()).installs();
    }

    std::string title(std::string_view profile_id) override {
        if (!titles_loaded_) {
            titles_loaded_ = true;
            for (const core::RegionProfile& p : load_profiles(executable_dir_)) {
                titles_[p.id] = p.title;
            }
        }
        const auto it = titles_.find(profile_id);
        return it != titles_.end() ? it->second : std::string();
    }

  private:
    fs::path executable_dir_;
    bool titles_loaded_ = false;
    std::map<std::string, std::string, std::less<>> titles_;
};

} // namespace

std::optional<fs::path> find_regions_dir(const fs::path& executable_dir) {
    std::vector<fs::path> candidates;
    fs::path dir = !executable_dir.empty() && !executable_dir.has_filename() ? executable_dir.parent_path()
                                                                             : executable_dir;
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        candidates.push_back(dir / "data" / "regions");
        candidates.push_back(dir / "share" / "dq8" / "regions");
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    candidates.push_back(fs::path("data") / "regions");
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::is_directory(c, ec)) {
            return c;
        }
    }
    return std::nullopt;
}

std::unique_ptr<WizardBackend> core_wizard_backend(fs::path executable_dir) {
    return std::make_unique<CoreWizardBackend>(std::move(executable_dir));
}

std::unique_ptr<LibraryBackend> registry_library(const fs::path& executable_dir) {
    return std::make_unique<RegistryLibrary>(executable_dir);
}

} // namespace dq8::launcher
