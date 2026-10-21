// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Builds small, valid ISO 9660 images in memory for the unit tests: one primary volume descriptor, a
// terminator, L and M path tables, both-byte-order fields, nested directories, ";1" file identifiers and
// files spanning sectors. Nothing in a built image comes from a game disc. The Layout returned by build()
// records where fields landed so a test can corrupt one and check the reader's reaction.
namespace dq8::test {

constexpr std::uint32_t sector = 2048;

struct Layout {
    std::uint64_t pvd_offset = 0;        // byte offset of the primary volume descriptor
    std::uint64_t terminator_offset = 0; // byte offset of the set terminator
    std::uint32_t path_table_l_lba = 0;
    std::uint32_t path_table_m_lba = 0;
    std::uint32_t path_table_size = 0;
    std::uint32_t root_lba = 0;
    // Byte offset of each directory record, keyed by '/'-separated path ("/BIN/TITLE.BIN"). Directories
    // are keyed by their path too ("/BIN"); the root's self record is "/".
    std::map<std::string, std::uint64_t> record_offset;
    std::map<std::string, std::uint32_t> extent_lba; // first LBA of each file's or directory's extent
};

class IsoBuilder {
  public:
    IsoBuilder();

    void set_system_identifier(std::string id) { system_id_ = std::move(id); }
    void set_volume_identifier(std::string id) { volume_id_ = std::move(id); }
    // Adds a directory ("/BIN"). Parents are created as needed.
    void add_directory(const std::string& path);
    // Adds a file ("/BIN/TITLE.BIN"); the identifier on disc gets ";1" appended, and a name without an
    // extension gets the "NAME.;1" form, as ISO 9660 level 1 mastering tools write it.
    void add_file(const std::string& path, std::vector<std::uint8_t> data);
    // Pads the image to this many sectors (e.g. to emulate DVD padding). 0 = exact.
    void set_total_sectors(std::uint32_t sectors) { total_sectors_ = sectors; }

    std::vector<std::uint8_t> build(Layout* layout = nullptr) const;
    void write(const std::filesystem::path& file, Layout* layout = nullptr) const;

  private:
    struct Node {
        std::string name;       // component as given ("TITLE.BIN")
        std::string identifier; // as recorded on disc ("TITLE.BIN;1", "BIN")
        bool directory = false;
        std::vector<std::uint8_t> data;
        std::vector<std::string> children; // child paths, insertion order
        std::string parent;                // parent path ("/" for top-level)
    };
    std::map<std::string, Node> nodes_;
    std::string system_id_ = "DQ8-NATIVE TEST";
    std::string volume_id_ = "SYNTHETIC";
    std::uint32_t total_sectors_ = 0;

    void ensure_directory(const std::string& path);
};

// Random bytes from a fixed seed: reproducible, never game data.
std::vector<std::uint8_t> seeded_bytes(std::uint32_t seed, std::size_t count);

// A fake PS2 boot executable: a valid ELF32 little-endian MIPS header with a main PT_LOAD at `load_address`
// and one zero-filesize PT_LOAD per entry of `overlay_memsz` at `window_base`, followed by `payload_size`
// seeded bytes. Nothing in it resembles real code.
std::vector<std::uint8_t> fake_boot_elf(std::uint32_t entry, std::uint32_t load_address,
                                        std::uint32_t main_memsz, std::uint32_t window_base,
                                        const std::vector<std::uint32_t>& overlay_memsz,
                                        std::size_t payload_size, std::uint32_t seed);

// A unique, empty temporary directory removed on destruction.
class TempDir {
  public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace dq8::test
