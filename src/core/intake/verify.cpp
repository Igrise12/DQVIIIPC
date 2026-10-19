// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/intake.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace dq8::core {

namespace {

constexpr std::uint64_t chunk_size = 1u << 20;

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\0')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

std::string hex32(std::uint32_t v) {
    return "0x" + to_hex(v);
}

} // namespace

SystemCnf parse_system_cnf(std::string_view text) {
    SystemCnf cnf;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (!trim(line).empty()) {
                throw Error(ErrorCode::system_cnf, "SYSTEM.CNF line '" + trim(line) + "' is not KEY = VALUE");
            }
            continue;
        }
        const std::string key = upper(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key.empty()) {
            throw Error(ErrorCode::system_cnf, "SYSTEM.CNF has a line with an empty key");
        }
        cnf.entries.emplace_back(key, value);
        if (key == "BOOT2") {
            cnf.boot2 = value;
        } else if (key == "VER") {
            cnf.ver = value;
        } else if (key == "VMODE") {
            cnf.vmode = value;
        }
    }
    if (cnf.boot2.empty()) {
        throw Error(ErrorCode::system_cnf, "SYSTEM.CNF has no BOOT2 line (not a PS2 disc?)");
    }
    const std::string device = "cdrom0:";
    if (cnf.boot2.size() <= device.size() || upper(cnf.boot2.substr(0, device.size())) != upper(device)) {
        throw Error(ErrorCode::system_cnf, "SYSTEM.CNF BOOT2 '" + cnf.boot2 + "' does not boot from cdrom0:");
    }
    std::string path = cnf.boot2.substr(device.size());
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(0, 1);
    }
    const std::size_t semicolon = path.rfind(';');
    if (semicolon != std::string::npos) {
        path.erase(semicolon);
    }
    if (path.empty()) {
        throw Error(ErrorCode::system_cnf, "SYSTEM.CNF BOOT2 '" + cnf.boot2 + "' names no file");
    }
    cnf.boot_path = path;
    return cnf;
}

FileDigest digest_entry(const iso::Image& image, const iso::Entry& entry, const ProgressFn& progress) {
    Sha1 sha;
    Crc32 crc;
    std::vector<std::uint8_t> chunk(
        static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, entry.size)));
    std::uint64_t done = 0;
    while (done < entry.size) {
        const std::uint64_t n = std::min<std::uint64_t>(chunk_size, entry.size - done);
        const std::span<std::uint8_t> part(chunk.data(), static_cast<std::size_t>(n));
        image.read(entry, done, part);
        sha.update(part);
        crc.update(part);
        done += n;
        if (progress) {
            progress(entry.path, done, entry.size);
        }
    }
    FileDigest d;
    d.size = entry.size;
    d.sha1 = to_hex(sha.finish());
    d.crc32 = to_hex(crc.value());
    return d;
}

FileDigest digest_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open '" + file.string() + "'");
    }
    Sha1 sha;
    Crc32 crc;
    std::vector<std::uint8_t> chunk(chunk_size);
    FileDigest d;
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::size_t got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        const std::span<const std::uint8_t> part(chunk.data(), got);
        sha.update(part);
        crc.update(part);
        d.size += got;
    }
    if (in.bad()) {
        throw Error(ErrorCode::io_read, "read error on '" + file.string() + "'");
    }
    d.sha1 = to_hex(sha.finish());
    d.crc32 = to_hex(crc.value());
    return d;
}

namespace {

ElfHeaders read_elf_headers(const iso::Image& image, const iso::Entry& boot) {
    std::vector<std::uint8_t> head(elf_header_size);
    if (boot.size < head.size()) {
        throw Error(ErrorCode::elf_format, "'" + boot.path + "' is too small to be an ELF file");
    }
    image.read(boot, 0, head);
    const std::size_t needed = elf_program_table_end(head);
    if (needed > boot.size) {
        throw Error(ErrorCode::elf_format,
                    "'" + boot.path + "' declares a program header table beyond its end");
    }
    head.resize(needed);
    image.read(boot, 0, head);
    return parse_elf_headers(head);
}

[[noreturn]] void mismatch(const std::string& what) {
    throw Error(ErrorCode::image_mismatch, what);
}

void cross_check_elf(const ElfHeaders& elf, const RegionProfile& p) {
    if (elf.machine != elf_em_mips) {
        mismatch("boot ELF machine type " + std::to_string(elf.machine) + " is not MIPS");
    }
    if (elf.entry != p.boot.entry) {
        mismatch("boot ELF entry point " + hex32(elf.entry) + " differs from the profile's " +
                 hex32(p.boot.entry));
    }
    const std::vector<const ElfSegment*> loads = elf.load_segments();
    const ElfSegment* main_segment = nullptr;
    std::vector<const ElfSegment*> window;
    for (const ElfSegment* s : loads) {
        if (s->filesz != 0 && s->vaddr == p.boot.load_address) {
            if (main_segment == nullptr) {
                main_segment = s;
            }
        } else if (s->filesz == 0 && s->vaddr == p.overlay_window.base) {
            window.push_back(s);
        } else if (s->filesz == 0 && s->memsz == 0) {
            continue; // an empty marker segment (the real disc has one at the window end)
        } else if (s->filesz == 0) {
            mismatch("boot ELF has an unexpected zero-filesize segment at " + hex32(s->vaddr));
        }
    }
    if (main_segment == nullptr) {
        mismatch("boot ELF has no load segment at " + hex32(p.boot.load_address));
    }
    if (main_segment->memsz != p.boot.main_memsz) {
        mismatch("boot ELF main segment memsz " + hex32(main_segment->memsz) +
                 " differs from the profile's " + hex32(p.boot.main_memsz));
    }
    if (window.size() != p.overlays.size()) {
        mismatch("boot ELF declares " + std::to_string(window.size()) + " overlay segments at " +
                 hex32(p.overlay_window.base) + " but the profile lists " +
                 std::to_string(p.overlays.size()));
    }
    for (std::size_t i = 0; i < window.size(); ++i) {
        if (window[i]->memsz != p.overlays[i].segment_memsz) {
            mismatch("overlay segment " + std::to_string(i) + " (" + p.overlays[i].name + ") has memsz " +
                     hex32(window[i]->memsz) + " but the profile records " +
                     hex32(p.overlays[i].segment_memsz));
        }
        if (window[i]->memsz > p.overlay_window.end - p.overlay_window.base) {
            mismatch("overlay segment " + std::to_string(i) + " does not fit the overlay window");
        }
    }
}

void cross_check_listed_file(const iso::Image& image, const std::string& path, std::uint64_t expected_size,
                             const char* kind) {
    const iso::Entry* e = image.find(path);
    if (e == nullptr) {
        mismatch(std::string("the profile lists ") + kind + " '" + path +
                 "' but the disc does not contain it");
    }
    if (e->is_directory()) {
        mismatch(std::string("the profile lists ") + kind + " '" + path +
                 "' but it is a directory on the disc");
    }
    if (expected_size != 0 && e->size != expected_size) {
        mismatch(std::string(kind) + " '" + path + "' is " + std::to_string(e->size) +
                 " bytes on the disc, " + std::to_string(expected_size) + " in the profile");
    }
}

} // namespace

VerifiedImage verify_image(const std::filesystem::path& image_path, std::span<const RegionProfile> profiles,
                           const ProgressFn& progress) {
    const iso::Image image = iso::Image::open(image_path);
    VerifiedImage v;
    v.image_path = image_path;
    v.image_size = image.image_size();
    v.volume_sectors = image.volume_sectors();

    const iso::Entry* cnf_entry = image.find("SYSTEM.CNF");
    if (cnf_entry == nullptr) {
        throw Error(ErrorCode::system_cnf, "the image has no SYSTEM.CNF: not a PlayStation 2 disc");
    }
    const std::vector<std::uint8_t> cnf_bytes = image.read_all(*cnf_entry, 64u << 10);
    v.system_cnf =
        parse_system_cnf(std::string_view(reinterpret_cast<const char*>(cnf_bytes.data()), cnf_bytes.size()));

    const iso::Entry* boot = image.find(v.system_cnf.boot_path);
    if (boot == nullptr || boot->is_directory()) {
        throw Error(ErrorCode::boot_file_missing, "SYSTEM.CNF boots '" + v.system_cnf.boot_path +
                                                      "' but the disc does not contain that file");
    }
    v.boot_path = boot->path;
    v.boot = digest_entry(image, *boot, progress);

    const RegionProfile* profile = find_profile_by_boot_sha1(profiles, v.boot.sha1);
    if (profile == nullptr) {
        std::string ids;
        for (const RegionProfile& p : profiles) {
            ids += ids.empty() ? p.id : ", " + p.id;
        }
        throw Error(ErrorCode::unknown_image,
                    "boot file '" + v.system_cnf.boot_path + "' (" + std::to_string(v.boot.size) +
                        " bytes, sha1 " + v.boot.sha1 + ", crc32 " + v.boot.crc32 +
                        ") matches no region profile" +
                        (ids.empty() ? std::string() : " (known: " + ids + ")") +
                        ". The image may be a different region, a modified or bad dump, "
                        "or a release this build does not know yet.");
    }
    v.profile = *profile;

    if (v.boot.size != profile->boot.size) {
        mismatch("boot file size " + std::to_string(v.boot.size) + " differs from the profile's " +
                 std::to_string(profile->boot.size) +
                 " although the SHA-1 matches; the profile is inconsistent");
    }
    if (v.boot.crc32 != profile->boot.crc32) {
        mismatch("boot file crc32 " + v.boot.crc32 + " differs from the profile's " + profile->boot.crc32 +
                 " although the SHA-1 matches; the profile is inconsistent");
    }
    if (iso::Image::normalize_path(v.system_cnf.boot_path) !=
        iso::Image::normalize_path(profile->boot.path)) {
        mismatch("SYSTEM.CNF boots '" + v.system_cnf.boot_path + "' but the profile expects '" +
                 profile->boot.path + "'");
    }

    v.elf = read_elf_headers(image, *boot);
    cross_check_elf(v.elf, *profile);

    for (const ProfileOverlay& o : profile->overlays) {
        cross_check_listed_file(image, o.path, o.size, "overlay");
    }
    for (const ProfileFile& f : profile->files) {
        cross_check_listed_file(image, f.path, f.size, "file");
    }

    if (v.image_size != profile->image_size) {
        v.warnings.push_back("image is " + std::to_string(v.image_size) + " bytes; the profile records " +
                             std::to_string(profile->image_size) + " (padded, trimmed or different dump?)");
    }
    if (static_cast<std::uint64_t>(v.volume_sectors) * iso::sector_size != v.image_size) {
        v.warnings.push_back("the volume descriptor declares " + std::to_string(v.volume_sectors) +
                             " sectors but the image holds " + std::to_string(image.image_sectors()));
    }
    if (!v.system_cnf.vmode.empty() && upper(v.system_cnf.vmode) != upper(profile->video_mode)) {
        v.warnings.push_back("SYSTEM.CNF VMODE is '" + v.system_cnf.vmode + "'; the profile records '" +
                             profile->video_mode + "'");
    }
    return v;
}

} // namespace dq8::core
