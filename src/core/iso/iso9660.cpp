// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/iso9660.h"

#include "dq8/core/error.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dq8::core::iso {

namespace {

constexpr std::uint32_t first_descriptor_lba = 16;
constexpr std::uint32_t max_descriptors = 64;
constexpr std::uint16_t max_depth = 16;
constexpr std::size_t min_record_length = 33;
constexpr std::size_t root_record_length = 34;

// Byte source behind an Image: a file or a memory buffer. Every read is bounds-checked by the caller.
class Source {
  public:
    virtual ~Source() = default;
    virtual std::uint64_t size() const noexcept = 0;
    virtual void read_at(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
};

class FileSource final : public Source {
  public:
    explicit FileSource(const std::filesystem::path& path) : path_(path.string()) {
        stream_.open(path, std::ios::binary);
        if (!stream_) {
            throw Error(ErrorCode::io_open, "cannot open '" + path_ + "'");
        }
        stream_.seekg(0, std::ios::end);
        const std::streamoff end = stream_.tellg();
        if (end < 0) {
            throw Error(ErrorCode::io_open, "cannot determine the size of '" + path_ + "'");
        }
        size_ = static_cast<std::uint64_t>(end);
    }

    std::uint64_t size() const noexcept override { return size_; }

    void read_at(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (out.empty()) {
            return;
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (static_cast<std::size_t>(stream_.gcount()) != out.size()) {
            throw Error(ErrorCode::io_read, "short read of " + std::to_string(out.size()) +
                                                " bytes at offset " + std::to_string(offset) + " in '" +
                                                path_ + "'");
        }
    }

  private:
    std::string path_;
    std::ifstream stream_;
    std::uint64_t size_ = 0;
};

class MemorySource final : public Source {
  public:
    explicit MemorySource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    void read_at(std::uint64_t offset, std::span<std::uint8_t> out) override {
        std::memcpy(out.data(), bytes_.data() + offset, out.size());
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

std::uint16_t le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t le32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// ECMA-119 "both-byte orders" fields (7.2.3 and 7.3.3): the value is recorded twice and must agree.
std::uint32_t both32(const std::uint8_t* p, const char* what, const std::string& where) {
    const std::uint32_t l = le32(p);
    const std::uint32_t m = be32(p + 4);
    if (l != m) {
        throw Error(ErrorCode::iso_endian_mismatch, std::string(what) + " in " + where +
                                                        " disagrees between byte orders (" +
                                                        std::to_string(l) + " vs " + std::to_string(m) + ")");
    }
    return l;
}
std::uint16_t both16(const std::uint8_t* p, const char* what, const std::string& where) {
    const std::uint16_t l = le16(p);
    const std::uint16_t m = be16(p + 2);
    if (l != m) {
        throw Error(ErrorCode::iso_endian_mismatch, std::string(what) + " in " + where +
                                                        " disagrees between byte orders (" +
                                                        std::to_string(l) + " vs " + std::to_string(m) + ")");
    }
    return l;
}

std::string trimmed(const std::uint8_t* p, std::size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) {
        s.pop_back();
    }
    return s;
}

char to_upper_ascii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// "TITLE.BIN;1" -> "TITLE.BIN", "DMYDATA.;1" -> "DMYDATA". Only the ISO 9660 version and the empty
// extension marker are removed; the case is kept.
std::string strip_version(std::string_view identifier) {
    std::string s(identifier);
    const std::size_t semicolon = s.rfind(';');
    if (semicolon != std::string::npos) {
        s.erase(semicolon);
    }
    if (s.size() > 1 && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

struct PathTableRecord {
    std::uint32_t extent = 0;
    std::uint16_t parent = 0; // 1-based record number
    std::string name;
};

std::vector<PathTableRecord> parse_path_table(std::span<const std::uint8_t> table, bool little_endian,
                                              const char* label) {
    std::vector<PathTableRecord> records;
    std::size_t offset = 0;
    while (offset < table.size()) {
        if (table.size() - offset < 8) {
            throw Error(ErrorCode::iso_path_table, std::string(label) + " path table record at offset " +
                                                       std::to_string(offset) + " is shorter than 8 bytes");
        }
        const std::uint8_t name_length = table[offset];
        const std::uint8_t ear_length = table[offset + 1];
        if (name_length == 0) {
            throw Error(ErrorCode::iso_path_table, std::string(label) + " path table record at offset " +
                                                       std::to_string(offset) + " has an empty name");
        }
        if (ear_length != 0) {
            throw Error(ErrorCode::iso_unsupported, std::string(label) + " path table record at offset " +
                                                        std::to_string(offset) +
                                                        " has an extended attribute record (unsupported)");
        }
        const std::size_t record_length = 8u + name_length + (name_length & 1u);
        if (table.size() - offset < record_length) {
            throw Error(ErrorCode::iso_path_table, std::string(label) + " path table record at offset " +
                                                       std::to_string(offset) + " overruns the table");
        }
        PathTableRecord rec;
        rec.extent = little_endian ? le32(&table[offset + 2]) : be32(&table[offset + 2]);
        rec.parent = little_endian ? le16(&table[offset + 6]) : be16(&table[offset + 6]);
        rec.name.assign(reinterpret_cast<const char*>(&table[offset + 8]), name_length);
        if (rec.parent == 0 || rec.parent > records.size() + 1) {
            throw Error(ErrorCode::iso_path_table, std::string(label) + " path table record " +
                                                       std::to_string(records.size() + 1) +
                                                       " names parent record " + std::to_string(rec.parent) +
                                                       ", which does not precede it");
        }
        records.push_back(std::move(rec));
        offset += record_length;
    }
    return records;
}

} // namespace

struct Image::Impl {
    std::unique_ptr<Source> source;
    std::uint64_t size = 0;
    VolumeInfo volume;
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> lookup; // normalized path -> index

    void read_at(std::uint64_t offset, std::span<std::uint8_t> out, const std::string& what) {
        if (offset > size || out.size() > size - offset) {
            throw Error(ErrorCode::iso_truncated, what + " lies beyond the end of the image (needs " +
                                                      std::to_string(offset + out.size()) +
                                                      " bytes, image has " + std::to_string(size) + ")");
        }
        source->read_at(offset, out);
    }

    std::vector<std::uint8_t> read_extent(std::uint32_t lba, std::uint32_t bytes, const std::string& what) {
        std::vector<std::uint8_t> data(bytes);
        read_at(static_cast<std::uint64_t>(lba) * sector_size, data, what);
        return data;
    }

    void scan_descriptors() {
        std::vector<std::uint8_t> sector(sector_size);
        bool have_primary = false;
        for (std::uint32_t lba = first_descriptor_lba;; ++lba) {
            if (lba >= first_descriptor_lba + max_descriptors) {
                throw Error(ErrorCode::iso_descriptor, "no volume descriptor set terminator within " +
                                                           std::to_string(max_descriptors) + " sectors");
            }
            const std::uint64_t offset = static_cast<std::uint64_t>(lba) * sector_size;
            if (offset + sector_size > size) {
                throw Error(ErrorCode::iso_descriptor,
                            have_primary ? "image ends before the volume descriptor set terminator"
                                         : "not an ISO 9660 image: too small to hold a volume descriptor");
            }
            source->read_at(offset, sector);
            if (std::memcmp(&sector[1], "CD001", 5) != 0) {
                throw Error(ErrorCode::iso_descriptor, "not an ISO 9660 image: descriptor at LBA " +
                                                           std::to_string(lba) +
                                                           " lacks the CD001 identifier");
            }
            if (sector[6] != 1) {
                throw Error(ErrorCode::iso_descriptor, "descriptor at LBA " + std::to_string(lba) +
                                                           " has unsupported version " +
                                                           std::to_string(sector[6]));
            }
            ++volume.descriptor_count;
            const std::uint8_t type = sector[0];
            if (type == 255) {
                break;
            }
            if (type == 0) {
                volume.has_boot_record = true;
            } else if (type == 2) {
                volume.has_supplementary = true;
            } else if (type == 1) {
                if (have_primary) {
                    continue; // a second primary descriptor is ignored, the first one wins
                }
                have_primary = true;
                parse_primary(sector, lba);
            }
        }
        if (!have_primary) {
            throw Error(ErrorCode::iso_descriptor, "not an ISO 9660 image: no primary volume descriptor");
        }
    }

    void parse_primary(const std::vector<std::uint8_t>& d, std::uint32_t lba) {
        const std::string where = "the primary volume descriptor (LBA " + std::to_string(lba) + ")";
        volume.system_identifier = trimmed(&d[8], 32);
        volume.volume_identifier = trimmed(&d[40], 32);
        volume.volume_space_size = both32(&d[80], "volume space size", where);
        volume.volume_set_size = both16(&d[120], "volume set size", where);
        volume.volume_sequence_number = both16(&d[124], "volume sequence number", where);
        volume.logical_block_size = both16(&d[128], "logical block size", where);
        volume.path_table_size = both32(&d[132], "path table size", where);
        volume.path_table_lba_l = le32(&d[140]);
        volume.path_table_lba_l_optional = le32(&d[144]);
        volume.path_table_lba_m = be32(&d[148]);
        volume.path_table_lba_m_optional = be32(&d[152]);
        volume.application_identifier = trimmed(&d[574], 128);
        if (volume.logical_block_size != sector_size) {
            throw Error(ErrorCode::iso_unsupported, "logical block size " +
                                                        std::to_string(volume.logical_block_size) +
                                                        " is not supported (only 2048)");
        }
        if (d[881] != 1) {
            throw Error(ErrorCode::iso_descriptor,
                        "file structure version " + std::to_string(d[881]) + " is not supported (only 1)");
        }
        const std::uint8_t* root = &d[156];
        if (root[0] != root_record_length || root[32] != 1 || root[33] != 0) {
            throw Error(ErrorCode::iso_descriptor, "the root directory record in " + where + " is malformed");
        }
        volume.root_lba = both32(root + 2, "root directory extent", where);
        volume.root_size = both32(root + 10, "root directory size", where);
        if ((root[25] & 0x02u) == 0) {
            throw Error(ErrorCode::iso_descriptor,
                        "the root directory record in " + where + " is not flagged as a directory");
        }
        const std::uint64_t volume_bytes = static_cast<std::uint64_t>(volume.volume_space_size) * sector_size;
        if (volume_bytes > size) {
            throw Error(ErrorCode::iso_truncated,
                        "the volume declares " + std::to_string(volume.volume_space_size) +
                            " sectors but the image holds only " + std::to_string(size / sector_size));
        }
    }

    // Parses one directory extent and appends its entries; recurses into subdirectories.
    void walk_directory(std::size_t dir_index, std::unordered_set<std::uint32_t>& visited) {
        const Entry dir = entries[dir_index]; // copy: entries may reallocate below
        if (!visited.insert(dir.lba).second) {
            throw Error(ErrorCode::iso_cycle, "directory '" + dir.path + "' at LBA " +
                                                  std::to_string(dir.lba) +
                                                  " was already visited (directory cycle)");
        }
        if (dir.depth >= max_depth) {
            throw Error(ErrorCode::iso_depth, "directory '" + dir.path + "' exceeds the depth limit of " +
                                                  std::to_string(max_depth));
        }
        const std::vector<std::uint8_t> data = read_extent(dir.lba, dir.size, "directory '" + dir.path + "'");
        std::size_t offset = 0;
        bool saw_self = false;
        bool saw_parent = false;
        std::vector<std::size_t> subdirectories;
        while (offset < data.size()) {
            const std::uint8_t length = data[offset];
            if (length == 0) {
                // Records never span sectors: the rest of this sector is padding.
                offset = (offset / sector_size + 1) * sector_size;
                continue;
            }
            const std::string where = "directory '" + dir.path + "' at offset " + std::to_string(offset);
            if (length < min_record_length || offset + length > data.size()) {
                throw Error(ErrorCode::iso_record,
                            "record in " + where + " has invalid length " + std::to_string(length));
            }
            const std::uint8_t* rec = &data[offset];
            const std::uint8_t name_length = rec[32];
            if (name_length == 0 || min_record_length + name_length > length) {
                throw Error(ErrorCode::iso_record,
                            "record in " + where + " has invalid name length " + std::to_string(name_length));
            }
            if (rec[1] != 0) {
                throw Error(ErrorCode::iso_unsupported,
                            "record in " + where + " has an extended attribute record (unsupported)");
            }
            const std::uint32_t extent = both32(rec + 2, "extent location", where);
            const std::uint32_t data_length = both32(rec + 10, "data length", where);
            const std::uint8_t flags = rec[25];
            if (rec[26] != 0 || rec[27] != 0) {
                throw Error(ErrorCode::iso_unsupported,
                            "record in " + where + " is interleaved (unsupported)");
            }
            both16(rec + 28, "volume sequence number", where);
            if ((flags & 0x80u) != 0) {
                throw Error(ErrorCode::iso_unsupported,
                            "record in " + where + " is a multi-extent file (unsupported)");
            }
            const std::uint8_t* name = rec + 33;
            if (name_length == 1 && name[0] == 0) {
                if (extent != dir.lba) {
                    throw Error(ErrorCode::iso_record, "the self entry of " + where + " points at LBA " +
                                                           std::to_string(extent) + ", not " +
                                                           std::to_string(dir.lba));
                }
                saw_self = true;
            } else if (name_length == 1 && name[0] == 1) {
                const std::uint32_t parent_lba = entries[dir.parent].lba;
                if (extent != parent_lba) {
                    throw Error(ErrorCode::iso_record, "the parent entry of " + where + " points at LBA " +
                                                           std::to_string(extent) + ", not " +
                                                           std::to_string(parent_lba));
                }
                saw_parent = true;
            } else {
                Entry e;
                e.identifier.assign(reinterpret_cast<const char*>(name), name_length);
                e.name = strip_version(e.identifier);
                e.path = (dir.path == "/" ? "/" : dir.path + "/") + e.name;
                e.lba = extent;
                e.size = data_length;
                e.flags = flags;
                e.parent = static_cast<std::uint32_t>(dir_index);
                e.depth = static_cast<std::uint16_t>(dir.depth + 1);
                const std::uint64_t end = static_cast<std::uint64_t>(e.lba) * sector_size + e.size;
                if (end > size) {
                    throw Error(ErrorCode::iso_truncated,
                                "'" + e.path + "' extends beyond the end of the image");
                }
                const std::string key = normalize_path(e.path);
                if (!lookup.emplace(key, entries.size()).second) {
                    throw Error(ErrorCode::iso_record, "'" + e.path + "' is recorded twice in its directory");
                }
                entries.push_back(std::move(e));
                if ((flags & 0x02u) != 0) {
                    subdirectories.push_back(entries.size() - 1);
                }
            }
            offset += length;
        }
        if (!saw_self || !saw_parent) {
            throw Error(ErrorCode::iso_record, "directory '" + dir.path + "' lacks its self or parent entry");
        }
        for (const std::size_t sub : subdirectories) {
            walk_directory(sub, visited);
        }
    }

    void cross_check_path_tables() {
        const std::string l_what = "the L path table";
        const std::string m_what = "the M path table";
        const std::vector<std::uint8_t> l_bytes =
            read_extent(volume.path_table_lba_l, volume.path_table_size, l_what);
        const std::vector<std::uint8_t> m_bytes =
            read_extent(volume.path_table_lba_m, volume.path_table_size, m_what);
        const std::vector<PathTableRecord> l = parse_path_table(l_bytes, true, "L");
        const std::vector<PathTableRecord> m = parse_path_table(m_bytes, false, "M");
        if (l.size() != m.size()) {
            throw Error(ErrorCode::iso_path_table, "the L and M path tables hold " +
                                                       std::to_string(l.size()) + " and " +
                                                       std::to_string(m.size()) + " records");
        }
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (l[i].extent != m[i].extent || l[i].parent != m[i].parent || l[i].name != m[i].name) {
                throw Error(ErrorCode::iso_path_table,
                            "L and M path table records " + std::to_string(i + 1) + " disagree");
            }
        }
        volume.path_table_directories = static_cast<std::uint32_t>(l.size());

        std::unordered_map<std::uint32_t, std::size_t> directory_by_lba;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].is_directory()) {
                directory_by_lba.emplace(entries[i].lba, i);
            }
        }
        if (l.size() != directory_by_lba.size()) {
            throw Error(ErrorCode::iso_path_table, "the path table lists " + std::to_string(l.size()) +
                                                       " directories but the directory tree holds " +
                                                       std::to_string(directory_by_lba.size()));
        }
        if (l.empty() || l[0].extent != volume.root_lba || l[0].parent != 1 ||
            l[0].name != std::string(1, '\0')) {
            throw Error(ErrorCode::iso_path_table, "the first path table record does not describe the root");
        }
        for (std::size_t i = 1; i < l.size(); ++i) {
            const auto it = directory_by_lba.find(l[i].extent);
            if (it == directory_by_lba.end()) {
                throw Error(ErrorCode::iso_path_table, "path table record " + std::to_string(i + 1) + " ('" +
                                                           l[i].name + "') points at LBA " +
                                                           std::to_string(l[i].extent) +
                                                           ", which is not a directory in the tree");
            }
            const Entry& e = entries[it->second];
            const std::uint32_t parent_lba = entries[e.parent].lba;
            if (l[l[i].parent - 1].extent != parent_lba || e.identifier != l[i].name) {
                throw Error(ErrorCode::iso_path_table, "path table record " + std::to_string(i + 1) + " ('" +
                                                           l[i].name + "') does not match directory '" +
                                                           e.path + "'");
            }
        }
    }

    void load() {
        size = source->size();
        scan_descriptors();
        Entry root;
        root.path = "/";
        root.lba = volume.root_lba;
        root.size = volume.root_size;
        root.flags = 0x02;
        root.parent = 0;
        root.depth = 0;
        entries.push_back(std::move(root));
        std::unordered_set<std::uint32_t> visited;
        walk_directory(0, visited);
        cross_check_path_tables();
    }
};

Image::Image(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;
Image::~Image() = default;

Image Image::open(const std::filesystem::path& path) {
    auto impl = std::make_unique<Impl>();
    impl->source = std::make_unique<FileSource>(path);
    impl->load();
    return Image(std::move(impl));
}

Image Image::from_bytes(std::vector<std::uint8_t> bytes) {
    auto impl = std::make_unique<Impl>();
    impl->source = std::make_unique<MemorySource>(std::move(bytes));
    impl->load();
    return Image(std::move(impl));
}

std::uint64_t Image::image_size() const noexcept {
    return impl_->size;
}

std::uint32_t Image::image_sectors() const noexcept {
    return static_cast<std::uint32_t>(impl_->size / sector_size);
}

std::uint32_t Image::volume_sectors() const noexcept {
    return impl_->volume.volume_space_size;
}

const VolumeInfo& Image::volume() const noexcept {
    return impl_->volume;
}

const std::vector<Entry>& Image::entries() const noexcept {
    return impl_->entries;
}

std::string Image::normalize_path(std::string_view path) {
    std::string out;
    std::string component;
    auto flush = [&]() {
        if (component.empty()) {
            return;
        }
        std::string name = strip_version(component);
        for (char& c : name) {
            c = to_upper_ascii(c);
        }
        if (!out.empty()) {
            out.push_back('/');
        }
        out += name;
        component.clear();
    };
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            flush();
        } else {
            component.push_back(c);
        }
    }
    flush();
    return out;
}

const Entry* Image::find(std::string_view path) const noexcept {
    const auto it = impl_->lookup.find(normalize_path(path));
    if (it == impl_->lookup.end()) {
        return normalize_path(path).empty() ? &impl_->entries[0] : nullptr;
    }
    return &impl_->entries[it->second];
}

const Entry& Image::require(std::string_view path) const {
    const Entry* e = find(path);
    if (e == nullptr) {
        throw Error(ErrorCode::iso_not_found, "'" + std::string(path) + "' is not on the disc");
    }
    return *e;
}

void Image::read(const Entry& entry, std::uint64_t offset, std::span<std::uint8_t> out) const {
    if (offset > entry.size || out.size() > entry.size - offset) {
        throw Error(ErrorCode::iso_range, "read of " + std::to_string(out.size()) + " bytes at offset " +
                                              std::to_string(offset) + " exceeds '" + entry.path + "' (" +
                                              std::to_string(entry.size) + " bytes)");
    }
    impl_->read_at(static_cast<std::uint64_t>(entry.lba) * sector_size + offset, out, "'" + entry.path + "'");
}

void Image::read_sectors(std::uint32_t lba, std::uint32_t count, std::span<std::uint8_t> out) const {
    if (out.size() != static_cast<std::uint64_t>(count) * sector_size) {
        throw Error(ErrorCode::iso_range, "read_sectors: buffer of " + std::to_string(out.size()) +
                                              " bytes does not hold " + std::to_string(count) + " sectors");
    }
    impl_->read_at(static_cast<std::uint64_t>(lba) * sector_size, out,
                   "sectors " + std::to_string(lba) + ".." + std::to_string(lba + count));
}

std::vector<std::uint8_t> Image::read_all(const Entry& entry, std::uint64_t max_bytes) const {
    if (entry.size > max_bytes) {
        throw Error(ErrorCode::iso_range, "'" + entry.path + "' is " + std::to_string(entry.size) +
                                              " bytes, more than the " + std::to_string(max_bytes) +
                                              " allowed for a whole-file read");
    }
    std::vector<std::uint8_t> data(entry.size);
    read(entry, 0, data);
    return data;
}

} // namespace dq8::core::iso
