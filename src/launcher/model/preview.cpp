// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/preview.h"

#include "dq8/core/version.h"
#include "model/setup_progress.h"

#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dq8::launcher::preview {

namespace {

using namespace std::chrono_literals;
using core::settings::Setting;
using core::settings::Type;

// Sleeps in small steps; false when cancelled meanwhile.
bool wait(std::chrono::milliseconds total, const std::atomic<bool>* cancel) {
    const auto end = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < end) {
        if (cancel != nullptr && cancel->load()) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return cancel == nullptr || !cancel->load();
}

class PreviewWizard final : public WizardBackend {
  public:
    VerifySummary verify(const std::filesystem::path& /*iso*/) override {
        std::this_thread::sleep_for(400ms);
        VerifySummary s;
        s.profile_id = "PREVIEW";
        s.title = "Preview disc (no game data)";
        s.boot_sha1 = "0000000000000000000000000000000000000000";
        s.image_size = 4'700'000'000;
        return s;
    }

    ToolchainReport check_toolchain() override {
        std::this_thread::sleep_for(300ms);
        ToolchainReport r;
        const auto tool = [](const char* path, const char* version) {
            core::toolchain::Tool t;
            t.found = true;
            t.path = path;
            t.version = version;
            return t;
        };
        r.toolchain.cc = tool("/usr/bin/cc", "preview");
        r.toolchain.cxx = tool("/usr/bin/c++", "preview");
        r.toolchain.cmake = tool("/usr/bin/cmake", "preview");
        r.toolchain.ninja = tool("/usr/bin/ninja", "preview");
        r.toolchain.compiler_id = "gnu";
        return r;
    }

    unsigned default_jobs() override { return 8; }

    core::setup::Result run_setup(const core::setup::Options& options) override {
        using core::setup::Event;
        using core::setup::State;
        core::setup::Result result;
        const auto send = [&](Event e) {
            if (options.on_event) {
                options.on_event(e);
            }
        };
        for (std::size_t i = 0; i < stage_count; ++i) {
            const core::setup::Stage stage = stage_at(i);
            const std::uint64_t steps = stage == core::setup::Stage::build ? 60 : 4;
            send({stage, State::start, 0, steps, {}, {}, {}});
            for (std::uint64_t n = 1; n <= steps; ++n) {
                if (!wait(stage == core::setup::Stage::build ? 80ms : 120ms, options.cancel)) {
                    send({stage, State::error, n, steps, {}, "cancelled", core::ErrorCode::setup_cancelled});
                    result.code = core::ErrorCode::setup_cancelled;
                    result.message = "the setup was cancelled";
                    return result;
                }
                send({stage,
                      State::progress,
                      n,
                      steps,
                      stage == core::setup::Stage::build ? "main" : "",
                      "step " + std::to_string(n),
                      {}});
            }
            send({stage, State::done, steps, steps, {}, {}, {}});
        }
        result.ok = true;
        result.profile_id = "PREVIEW";
        return result;
    }
};

class PreviewLibrary final : public LibraryBackend {
  public:
    PreviewLibrary(std::filesystem::path file, std::filesystem::path dir)
        : file_(std::move(file)), dir_(std::move(dir)) {}

    std::vector<core::registry::Install> installs() override {
        core::registry::Install i;
        i.profile_id = "PREVIEW";
        i.iso = file_;
        i.module_dir = dir_;
        i.manifest_sha1 = "0000000000000000000000000000000000000000";
        i.built_at = "2026-10-22T12:00:00Z";
        i.host_version = core::version();
        return {i};
    }

    std::string title(std::string_view /*profile_id*/) override { return "Preview disc (no game data)"; }

  private:
    std::filesystem::path file_;
    std::filesystem::path dir_;
};

class PreviewDiagnostics final : public DiagnosticsExporter {
  public:
    ExportOutcome write(const std::filesystem::path& /*file*/, const std::atomic<bool>& cancel) override {
        ExportOutcome out;
        if (!wait(600ms, &cancel)) {
            return out;
        }
        out.ok = true;
        out.output = "preview: nothing was written";
        return out;
    }
};

Setting make(std::string key, Type type, core::settings::Value def, std::string category) {
    Setting s;
    s.key = std::move(key);
    s.type = type;
    s.default_value = std::move(def);
    s.label = s.key + ".label";
    s.description = s.key + ".description";
    s.category = std::move(category);
    return s;
}

std::unique_ptr<SettingsBackend> sample_settings() {
    std::vector<Setting> rows;
    Setting mode = make("display.window_mode", Type::choice, std::string("windowed"), "display");
    mode.choices = {"windowed", "borderless", "fullscreen"};
    rows.push_back(mode);
    Setting width = make("display.width", Type::integer, std::int64_t{1280}, "display");
    width.min = 640;
    width.max = 7680;
    width.conditions = {"display.window_mode=windowed"};
    rows.push_back(width);
    rows.push_back(make("display.vsync", Type::boolean, true, "display"));
    Setting res = make("display.internal_resolution", Type::choice, std::string("1x"), "display");
    res.choices = {"1x", "2x", "3x", "4x"};
    res.requires_restart = true;
    rows.push_back(res);
    Setting volume = make("audio.volume", Type::integer, std::int64_t{100}, "audio");
    volume.max = 100;
    rows.push_back(volume);
    Setting latency = make("audio.latency_ms", Type::integer, std::int64_t{64}, "audio");
    latency.min = 10;
    latency.max = 1000;
    latency.advanced = true;
    latency.requires_restart = true;
    rows.push_back(latency);
    rows.push_back(make("input.rumble", Type::boolean, true, "input"));
    Setting strength = make("input.rumble_strength", Type::integer, std::int64_t{100}, "input");
    strength.max = 100;
    strength.conditions = {"input.rumble=true"};
    rows.push_back(strength);
    Setting save_dir = make("game.save_dir", Type::path, std::string(), "game");
    save_dir.requires_restart = true;
    rows.push_back(save_dir);
    std::map<std::string, std::string, std::less<>> strings{
        {"category.display", "Display"},
        {"category.audio", "Audio"},
        {"category.input", "Input"},
        {"category.game", "Game"},
        {"display.window_mode.label", "Window mode"},
        {"display.window_mode.choices.windowed", "Windowed"},
        {"display.window_mode.choices.borderless", "Borderless fullscreen"},
        {"display.window_mode.choices.fullscreen", "Exclusive fullscreen"},
        {"display.width.label", "Window width"},
        {"display.vsync.label", "Vertical sync"},
        {"display.internal_resolution.label", "Internal resolution"},
        {"audio.volume.label", "Volume"},
        {"audio.latency_ms.label", "Audio latency"},
        {"input.rumble.label", "Vibration"},
        {"input.rumble_strength.label", "Vibration strength"},
        {"game.save_dir.label", "Save folder"},
    };
    return std::make_unique<MemorySettings>(std::move(rows), std::move(strings), "preview-settings.toml");
}

} // namespace

std::unique_ptr<WizardBackend> wizard_backend() {
    return std::make_unique<PreviewWizard>();
}

std::unique_ptr<LibraryBackend> library_backend(std::filesystem::path existing_file,
                                                std::filesystem::path existing_dir) {
    return std::make_unique<PreviewLibrary>(std::move(existing_file), std::move(existing_dir));
}

std::unique_ptr<SettingsBackend> settings_backend(const std::filesystem::path& executable_dir) {
    try {
        return open_store_settings(executable_dir);
    } catch (...) {
        return sample_settings();
    }
}

std::unique_ptr<DiagnosticsExporter> diagnostics_exporter() {
    return std::make_unique<PreviewDiagnostics>();
}

ProcessRunner game_runner() {
    return [](const core::ProcessOptions& options) {
        core::ProcessResult r;
        const auto line = [&](const std::string& text) {
            if (options.on_output) {
                options.on_output(core::OutputStream::out, text + "\n");
            }
        };
        line(std::string(R"({"ev":"started","version":")") + core::version() +
             R"(","profile":"PREVIEW","pid":1,"window":true})");
        if (options.on_output) {
            options.on_output(core::OutputStream::err, "preview: no game runs; this is a stand-in\n");
        }
        if (!wait(350ms, options.cancel)) {
            r.cancelled = true;
            return r;
        }
        line(R"({"ev":"first_frame","ms":350})");
        for (int second = 1; second <= 30; ++second) {
            if (!wait(1000ms, options.cancel)) {
                r.cancelled = true;
                return r;
            }
            line(R"({"ev":"fps","fps":59.94,"speed":1.0})");
            if (second == 2) {
                line(
                    R"({"ev":"warning","code":"preview","message":"a warning, as dq8-game would report one"})");
            }
        }
        line(R"({"ev":"exit","status":0,"reason":"window-closed"})");
        r.exit_code = 0;
        return r;
    };
}

} // namespace dq8::launcher::preview
