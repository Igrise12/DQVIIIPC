// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "iso_builder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dq8::test {

namespace {

void put16_both(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
void put32_le(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
void put32_be(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[3 - i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
void put32_both(std::uint8_t* p, std::uint32_t v) {
    put32_le(p, v);
    put32_be(p + 4, v);
}
void put16_le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put16_be(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void put_padded(std::uint8_t* p, std::size_t n, const std::string& s) {
    std::memset(p, ' ', n);
    std::memcpy(p, s.data(), std::min(n, s.size()));
}

// GCC's -Wnull-dereference cannot see that these buffers are never empty; an explicit check keeps -Werror
// builds quiet on every compiler.
std::uint8_t* base_of(std::vector<std::uint8_t>& v) {
    std::uint8_t* p = v.data();
    if (p == nullptr) {
        throw std::logic_error("IsoBuilder: empty buffer");
    }
    return p;
}

std::uint32_t sectors_for(std::size_t bytes) {
    return static_cast<std::uint32_t>((bytes + sector - 1) / sector);
}

// A directory record for `identifier`. Both-byte-order fields are written in both orders.
std::vector<std::uint8_t> make_record(const std::string& identifier, std::uint32_t lba, std::uint32_t size,
                                      bool directory) {
    const std::size_t name_len = identifier.size();
    const std::size_t len = 33 + name_len + ((name_len % 2 == 0) ? 1 : 0);
    std::vector<std::uint8_t> record(len, 0);
    std::uint8_t* r = base_of(record);
    r[0] = static_cast<std::uint8_t>(len);
    r[1] = 0;
    put32_both(r + 2, lba);
    put32_both(r + 10, size);
    // recording date: 2026-01-01 00:00:00, offset 0
    r[18] = 126;
    r[19] = 1;
    r[20] = 1;
    r[25] = directory ? 0x02 : 0x00;
    r[26] = 0;
    r[27] = 0;
    put16_both(r + 28, 1);
    r[32] = static_cast<std::uint8_t>(name_len);
    std::memcpy(r + 33, identifier.data(), name_len);
    return record;
}

} // namespace

IsoBuilder::IsoBuilder() {
    Node root;
    root.name = "";
    root.identifier = std::string(1, '\0');
    root.directory = true;
    root.parent = "";
    nodes_["/"] = root;
}

void IsoBuilder::ensure_directory(const std::string& path) {
    if (path == "/" || nodes_.count(path) != 0) {
        return;
    }
    const std::size_t slash = path.rfind('/');
    const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
    ensure_directory(parent);
    Node n;
    n.name = path.substr(slash + 1);
    n.identifier = n.name;
    n.directory = true;
    n.parent = parent;
    nodes_[path] = n;
    nodes_[parent].children.push_back(path);
}

void IsoBuilder::add_directory(const std::string& path) {
    ensure_directory(path);
}

void IsoBuilder::add_file(const std::string& path, std::vector<std::uint8_t> data) {
    if (path.empty() || path[0] != '/' || nodes_.count(path) != 0) {
        throw std::invalid_argument("IsoBuilder::add_file: bad or duplicate path " + path);
    }
    const std::size_t slash = path.rfind('/');
    const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
    ensure_directory(parent);
    Node n;
    n.name = path.substr(slash + 1);
    n.identifier = (n.name.find('.') == std::string::npos ? n.name + "." : n.name) + ";1";
    n.directory = false;
    n.data = std::move(data);
    n.parent = parent;
    nodes_[path] = n;
    nodes_[parent].children.push_back(path);
}

std::vector<std::uint8_t> IsoBuilder::build(Layout* layout) const {
    Layout lay;
    // Directory order: breadth-first, as the path table wants it (by depth, then parent, then name).
    std::vector<std::string> directories{"/"};
    for (std::size_t i = 0; i < directories.size(); ++i) {
        std::vector<std::string> subs;
        for (const std::string& child : nodes_.at(directories[i]).children) {
            if (nodes_.at(child).directory) {
                subs.push_back(child);
            }
        }
        std::sort(subs.begin(), subs.end());
        directories.insert(directories.end(), subs.begin(), subs.end());
    }
    std::map<std::string, std::uint16_t> dir_number; // 1-based path table record number
    for (std::size_t i = 0; i < directories.size(); ++i) {
        dir_number[directories[i]] = static_cast<std::uint16_t>(i + 1);
    }

    // Path table size (same for L and M).
    std::uint32_t pt_size = 0;
    for (const std::string& d : directories) {
        const std::size_t n = nodes_.at(d).identifier.size();
        pt_size += static_cast<std::uint32_t>(8 + n + (n & 1));
    }

    // Lay out extents: descriptors at 16/17, path tables at 18/19, then directories, then files.
    std::uint32_t next_lba = 18;
    lay.path_table_l_lba = next_lba;
    next_lba += sectors_for(pt_size);
    lay.path_table_m_lba = next_lba;
    next_lba += sectors_for(pt_size);
    lay.path_table_size = pt_size;

    // Directory extents: compute each directory's record list size first (depends on children's sizes,
    // which for subdirectories depend on their own extents, so do sizes in one pass, LBAs in another).
    std::map<std::string, std::uint32_t> dir_size;
    for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
        const Node& d = nodes_.at(*it);
        std::uint32_t size = 34 + 34; // self and parent records
        for (const std::string& child : d.children) {
            const std::size_t n = nodes_.at(child).identifier.size();
            size += static_cast<std::uint32_t>(33 + n + ((n % 2 == 0) ? 1 : 0));
        }
        dir_size[*it] = size;
    }
    for (const std::string& d : directories) {
        lay.extent_lba[d] = next_lba;
        next_lba += sectors_for(dir_size[d]);
    }
    lay.root_lba = lay.extent_lba["/"];
    for (const std::string& d : directories) {
        for (const std::string& child : nodes_.at(d).children) {
            const Node& c = nodes_.at(child);
            if (!c.directory) {
                lay.extent_lba[child] = next_lba;
                next_lba += std::max<std::uint32_t>(1, sectors_for(c.data.size()));
            }
        }
    }
    const std::uint32_t volume_sectors = total_sectors_ == 0 ? next_lba : std::max(total_sectors_, next_lba);
    std::vector<std::uint8_t> image(static_cast<std::size_t>(volume_sectors) * sector, 0);
    std::uint8_t* const img = base_of(image);

    // Primary volume descriptor.
    {
        std::uint8_t* d = img + 16 * sector;
        lay.pvd_offset = 16 * sector;
        d[0] = 1;
        std::memcpy(&d[1], "CD001", 5);
        d[6] = 1;
        put_padded(&d[8], 32, system_id_);
        put_padded(&d[40], 32, volume_id_);
        put32_both(&d[80], volume_sectors);
        put16_both(&d[120], 1);
        put16_both(&d[124], 1);
        put16_both(&d[128], static_cast<std::uint16_t>(sector));
        put32_both(&d[132], pt_size);
        put32_le(&d[140], lay.path_table_l_lba);
        put32_le(&d[144], 0);
        put32_be(&d[148], lay.path_table_m_lba);
        put32_be(&d[152], 0);
        const std::vector<std::uint8_t> root =
            make_record(std::string(1, '\0'), lay.root_lba, dir_size["/"], true);
        std::memcpy(&d[156], root.data(), root.size());
        put_padded(&d[190], 128, "");
        put_padded(&d[318], 128, "DQ8-NATIVE UNIT TESTS");
        put_padded(&d[446], 128, "");
        put_padded(&d[574], 128, "DQ8-NATIVE");
        put_padded(&d[702], 37, "");
        put_padded(&d[739], 37, "");
        put_padded(&d[776], 37, "");
        d[881] = 1;
    }
    // Terminator.
    {
        std::uint8_t* d = img + 17 * sector;
        lay.terminator_offset = 17 * sector;
        d[0] = 255;
        std::memcpy(&d[1], "CD001", 5);
        d[6] = 1;
    }
    // Path tables.
    for (int which = 0; which < 2; ++which) {
        const bool little = which == 0;
        std::uint8_t* p =
            img + static_cast<std::size_t>(little ? lay.path_table_l_lba : lay.path_table_m_lba) * sector;
        std::size_t off = 0;
        for (const std::string& d : directories) {
            const Node& n = nodes_.at(d);
            const std::uint16_t parent = d == "/" ? 1 : dir_number[n.parent];
            p[off] = static_cast<std::uint8_t>(n.identifier.size());
            p[off + 1] = 0;
            if (little) {
                put32_le(&p[off + 2], lay.extent_lba[d]);
                put16_le(&p[off + 6], parent);
            } else {
                put32_be(&p[off + 2], lay.extent_lba[d]);
                put16_be(&p[off + 6], parent);
            }
            std::memcpy(&p[off + 8], n.identifier.data(), n.identifier.size());
            off += 8 + n.identifier.size() + (n.identifier.size() & 1);
        }
    }
    // Directories and files.
    for (const std::string& d : directories) {
        const Node& n = nodes_.at(d);
        std::uint8_t* base = img + static_cast<std::size_t>(lay.extent_lba[d]) * sector;
        std::size_t off = 0;
        auto emit = [&](const std::string& key, const std::vector<std::uint8_t>& rec) {
            std::memcpy(base + off, rec.data(), rec.size());
            if (!key.empty()) {
                lay.record_offset[key] = static_cast<std::uint64_t>(lay.extent_lba[d]) * sector + off;
            }
            off += rec.size();
        };
        emit(d == "/" ? "/" : "", make_record(std::string(1, '\0'), lay.extent_lba[d], dir_size[d], true));
        const std::string parent = d == "/" ? "/" : n.parent;
        emit("", make_record(std::string(1, '\1'), lay.extent_lba[parent], dir_size[parent], true));
        for (const std::string& child : n.children) {
            const Node& c = nodes_.at(child);
            const std::uint32_t size =
                c.directory ? dir_size[child] : static_cast<std::uint32_t>(c.data.size());
            emit(child, make_record(c.identifier, lay.extent_lba[child], size, c.directory));
            if (!c.directory && !c.data.empty()) {
                std::memcpy(img + static_cast<std::size_t>(lay.extent_lba[child]) * sector, c.data.data(),
                            c.data.size());
            }
        }
    }
    if (layout != nullptr) {
        *layout = lay;
    }
    return image;
}

void IsoBuilder::write(const std::filesystem::path& file, Layout* layout) const {
    const std::vector<std::uint8_t> image = build(layout);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + file.string());
    }
}

std::vector<std::uint8_t> seeded_bytes(std::uint32_t seed, std::size_t count) {
    // xorshift32: deterministic and dependency-free.
    std::uint32_t x = seed == 0 ? 0x9E3779B9u : seed;
    std::vector<std::uint8_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = static_cast<std::uint8_t>(x);
    }
    return out;
}

std::vector<std::uint8_t> fake_boot_elf(std::uint32_t entry, std::uint32_t load_address,
                                        std::uint32_t main_memsz, std::uint32_t window_base,
                                        const std::vector<std::uint32_t>& overlay_memsz,
                                        std::size_t payload_size, std::uint32_t seed) {
    const std::uint16_t phnum = static_cast<std::uint16_t>(1 + overlay_memsz.size());
    const std::size_t header_end = 52 + static_cast<std::size_t>(phnum) * 32;
    const std::size_t payload_offset = (header_end + 127) & ~static_cast<std::size_t>(127);
    std::vector<std::uint8_t> file(payload_offset + payload_size, 0);
    std::uint8_t* const elf = base_of(file);
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 1;            // ELFCLASS32
    elf[5] = 1;            // ELFDATA2LSB
    elf[6] = 1;            // EV_CURRENT
    put16_le(elf + 16, 2); // ET_EXEC
    put16_le(elf + 18, 8); // EM_MIPS
    put32_le(elf + 20, 1);
    put32_le(elf + 24, entry);
    put32_le(elf + 28, 52);
    put32_le(elf + 32, 0);
    put32_le(elf + 36, 0x20924000u);
    put16_le(elf + 40, 52);
    put16_le(elf + 42, 32);
    put16_le(elf + 44, phnum);
    put16_le(elf + 46, 40);
    put16_le(elf + 48, 0);
    put16_le(elf + 50, 0);
    auto phdr = [&](std::size_t i, std::uint32_t offset, std::uint32_t vaddr, std::uint32_t filesz,
                    std::uint32_t memsz) {
        std::uint8_t* p = elf + 52 + i * 32;
        put32_le(p, 1);
        put32_le(p + 4, offset);
        put32_le(p + 8, vaddr);
        put32_le(p + 12, vaddr);
        put32_le(p + 16, filesz);
        put32_le(p + 20, memsz);
        put32_le(p + 24, 7);
        put32_le(p + 28, 0x80);
    };
    phdr(0, static_cast<std::uint32_t>(payload_offset), load_address,
         static_cast<std::uint32_t>(payload_size), main_memsz);
    for (std::size_t i = 0; i < overlay_memsz.size(); ++i) {
        phdr(i + 1, static_cast<std::uint32_t>(payload_offset + payload_size), window_base, 0,
             overlay_memsz[i]);
    }
    const std::vector<std::uint8_t> payload = seeded_bytes(seed, payload_size);
    std::copy(payload.begin(), payload.end(), file.begin() + static_cast<std::ptrdiff_t>(payload_offset));
    return file;
}

TempDir::TempDir() {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("dq8-unit-" + std::to_string(static_cast<long long>(stamp)) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

} // namespace dq8::test
