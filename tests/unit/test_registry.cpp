// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// M5 session A: the build registry's file (dq8/core/registry.h).
#include "dq8/core/error.h"
#include "dq8/core/registry.h"
#include "support/iso_builder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

using Catch::Matchers::ContainsSubstring;
namespace registry = dq8::core::registry;
namespace fs = std::filesystem;

TEST_CASE("registry: installs round-trip through installs.toml", "[m5][registry]") {
    const dq8::test::TempDir tmp;
    const fs::path file = tmp.path() / "data" / "installs.toml";
    CHECK(registry::Registry::load(file).installs().empty()); // a missing file is an empty registry

    registry::Registry r;
    r.upsert({"SLUS_21207", tmp.path() / "Game" / "disc image.iso",
              tmp.path() / "build" / "modules" / "SLUS_21207", "0123456789abcdef0123456789abcdef01234567",
              "2026-10-22T12:00:00Z", "0.0.1"});
    r.upsert({"TEST_00001", "t.iso", "mods", "", "", "0.0.1"});
    r.save(file);

    const registry::Registry back = registry::Registry::load(file);
    REQUIRE(back.installs().size() == 2);
    const registry::Install* i = back.find("SLUS_21207");
    REQUIRE(i != nullptr);
    CHECK(i->iso == tmp.path() / "Game" / "disc image.iso");
    CHECK(i->module_dir == tmp.path() / "build" / "modules" / "SLUS_21207");
    CHECK(i->manifest_sha1 == "0123456789abcdef0123456789abcdef01234567");
    CHECK(i->built_at == "2026-10-22T12:00:00Z");
    CHECK(i->host_version == "0.0.1");
    CHECK(back.installs()[1].profile_id == "TEST_00001");
}

TEST_CASE("registry: a damaged file is registry-parse with its line; unknown keys are ignored",
          "[m5][registry]") {
    const dq8::test::TempDir tmp;
    const fs::path file = tmp.path() / "installs.toml";
    std::ofstream(file)
        << "schema_version = 1\n[[install]]\nprofile = \"A\"\nmodule_dir = \"m\"\nlater = 3\n";
    CHECK(registry::Registry::load(file).find("A") != nullptr);

    std::ofstream(file, std::ios::trunc) << "schema_version = 1\n[[install]\n";
    try {
        (void)registry::Registry::load(file);
        FAIL("no error");
    } catch (const dq8::core::Error& e) {
        CHECK(e.code() == dq8::core::ErrorCode::registry_parse);
        CHECK_THAT(e.what(), ContainsSubstring("installs.toml:2:"));
    }
    std::ofstream(file, std::ios::trunc) << "[[install]]\nprofile = 7\nmodule_dir = \"m\"\n";
    CHECK_THROWS_AS(registry::Registry::load(file), dq8::core::Error);
    std::ofstream(file, std::ios::trunc) << "[[install]]\nprofile = \"A\"\n";
    CHECK_THROWS_AS(registry::Registry::load(file), dq8::core::Error); // module_dir is required
}
