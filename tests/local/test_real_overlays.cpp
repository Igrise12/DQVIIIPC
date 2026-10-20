// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// Parses the six real overlays from the user's disc (DQ8_TEST_ISO) and checks the identities the region
// profile records: 64 + text + data + bss == the ELF segment's memsz, the integrity values per row, and
// that the header names are the lowercased file names. Nothing from the disc is written anywhere.
#include "dq8/core/elf_headers.h"
#include "dq8/core/intake.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/mwo3.h"
#include "dq8/core/region_profile.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path test_iso() {
    const char* env = std::getenv("DQ8_TEST_ISO");
    if (env == nullptr || *env == '\0') {
        SKIP("DQ8_TEST_ISO is not set");
    }
    return env;
}

std::filesystem::path regions_dir() {
    return std::filesystem::path(DQ8_SOURCE_DIR) / "data" / "regions";
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

} // namespace

TEST_CASE("the six real overlays parse as MWo3 containers that match the profile and the ELF",
          "[local][mwo3]") {
    const auto iso = test_iso();
    const auto profiles = dq8::core::load_region_profiles(regions_dir());
    const auto v = dq8::core::verify_image(iso, profiles);
    REQUIRE(v.profile.id == "SLUS_21207");
    REQUIRE(v.profile.overlays.size() == 6);
    REQUIRE(v.overlay_headers.size() == 6);
    const auto loads = v.elf.load_segments();
    REQUIRE(loads.size() == 8);

    const auto image = dq8::core::iso::Image::open(iso);
    const dq8::core::Mwo3Window window{v.profile.overlay_window.base, v.profile.overlay_window.end};
    for (std::size_t i = 0; i < v.profile.overlays.size(); ++i) {
        const dq8::core::ProfileOverlay& o = v.profile.overlays[i];
        INFO("overlay " << o.name);
        // Parse the file straight from the image, independently of verify_image().
        const auto bytes = image.read_all(image.require(o.path), window.end - window.base);
        const auto parsed = dq8::core::parse_mwo3(bytes, window);
        const dq8::core::Mwo3Header& h = parsed.header;
        CHECK(h.magic == dq8::core::mwo3_magic);
        CHECK(h.overlay_id == o.id);
        CHECK(h.load_address == window.base);
        CHECK(h.text_size == o.text_size);
        CHECK(h.data_size == o.data_size);
        CHECK(h.bss_size == o.bss_size);
        CHECK(h.table_entry_count() == o.table_entries);
        CHECK(h.file_size() == bytes.size());
        CHECK(h.file_size() == o.size);
        CHECK(parsed.image.size() == bytes.size() - 64);
        // The identity with the boot ELF: header + text + data + bss == the segment's memsz.
        REQUIRE(o.segment_index < loads.size());
        CHECK(h.memsz() == loads[o.segment_index]->memsz);
        CHECK(h.memsz() == o.segment_memsz);
        CHECK(loads[o.segment_index]->filesz == 0);
        CHECK(loads[o.segment_index]->vaddr == window.base);
        CHECK(h.end_address() <= window.end);
        // The constructor table sits inside the data range, strictly after its start.
        CHECK(h.table_start > h.data_start());
        CHECK(h.table_end < h.bss_start());
        // The header's name is the lowercased file name ("title.bin" for BIN/TITLE.BIN).
        CHECK(h.name == lower(std::filesystem::path(o.path).filename().string()));
        // verify_image() read the same header.
        CHECK(v.overlay_headers[i].raw_words == h.raw_words);
        CHECK(v.overlay_headers[i].name == h.name);
        // The loader index and the name table agree.
        CHECK(v.profile.overlay_window.name_table_order.at(o.index) == o.name);
    }
    // Spike S3's facts: ids 1..6 in segment order, six distinct names, the largest overlay fills the window.
    std::uint32_t largest = 0;
    for (const auto& h : v.overlay_headers) {
        largest = std::max(largest, h.end_address());
    }
    CHECK(largest == window.end);
}
