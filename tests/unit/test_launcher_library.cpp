// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The Play page's list of registered builds.
#include "model/library.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace dq8::launcher;
using dq8::core::ErrorCode;
using dq8::core::registry::Install;

namespace {

class FakeLibrary final : public LibraryBackend {
  public:
    std::vector<Install> list;
    std::optional<ErrorCode> error;
    bool stub = false;

    std::vector<Install> installs() override {
        if (stub) {
            throw std::logic_error("registry: load is not implemented (M5 session A)");
        }
        if (error) {
            throw dq8::core::Error(*error, "installs.toml:3: bad value");
        }
        return list;
    }
    std::string title(std::string_view profile_id) override {
        if (profile_id == "NOTITLE") {
            throw dq8::core::Error(ErrorCode::io_open, "no profiles");
        }
        return "Title of " + std::string(profile_id);
    }
};

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("dq8-launcher-library-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path / "modules");
        std::ofstream(path / "disc.iso") << "x";
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("launcher: the library lists the registered builds", "[launcher][library]") {
    TempDir dir;
    auto backend = std::make_unique<FakeLibrary>();
    FakeLibrary* fake = backend.get();
    fake->list = {
        {"SLUS_21207", dir.path / "disc.iso", dir.path / "modules", "abc", "2026-10-22T10:00:00Z", "0.0.1"},
        {"NOTITLE", dir.path / "gone.iso", dir.path / "nowhere", "def", "2026-10-21T10:00:00Z", "0.0.0"}};
    LibraryModel m(std::move(backend), "0.0.1");
    CHECK(m.empty());
    m.refresh();
    REQUIRE(m.entries().size() == 2);
    CHECK_FALSE(m.problem());
    const LibraryEntry& a = m.entries()[0];
    CHECK(a.title == "Title of SLUS_21207");
    CHECK(a.image_found);
    CHECK(a.modules_found);
    CHECK_FALSE(a.other_version);
    const LibraryEntry& b = m.entries()[1];
    CHECK(b.title.empty());
    CHECK_FALSE(b.image_found);
    CHECK_FALSE(b.modules_found);
    CHECK(b.other_version);

    const auto play = m.play_request("/bin/dq8-game", "/cfg/settings.toml");
    REQUIRE(play);
    CHECK(play->iso == dir.path / "disc.iso");
    CHECK(play->module_dir == dir.path / "modules");
    CHECK(play->config_file == "/cfg/settings.toml");
    CHECK(play->profile_id == "SLUS_21207");
    CHECK_FALSE(m.play_request("", "/cfg/settings.toml")); // no dq8-game found

    m.select(1);
    CHECK(m.current()->install.profile_id == "NOTITLE");
    CHECK_FALSE(m.play_request("/bin/dq8-game", {})); // its files are gone
    m.select(7);                                      // out of range: unchanged
    CHECK(m.selected() == 1);

    // The selection survives a refresh.
    m.refresh();
    CHECK(m.current()->install.profile_id == "NOTITLE");
}

TEST_CASE("launcher: registry errors reach the page", "[launcher][library]") {
    auto backend = std::make_unique<FakeLibrary>();
    FakeLibrary* fake = backend.get();
    fake->error = ErrorCode::registry_parse;
    LibraryModel m(std::move(backend), "0.0.1");
    m.refresh();
    CHECK(m.empty());
    REQUIRE(m.problem());
    CHECK(*m.problem()->code == ErrorCode::registry_parse);
    CHECK_FALSE(m.problem()->action.empty());

    fake->error.reset();
    fake->stub = true;
    m.refresh();
    REQUIRE(m.problem());
    CHECK_FALSE(m.problem()->code);
    CHECK_FALSE(m.current());
}
