// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// dq8ctl: command-line front end over libdq8core. Same operations as the launcher, machine-readable output.
//
// Exit status: 0 success, 1 the image does not match (verify) or an extracted file's hash differs (extract),
// 2 usage or I/O error. With --json one JSON object is printed on stdout, {"ok": true, ...} or
// {"ok": false, "error": {"code": ..., "message": ...}}; that object is the seam the launcher consumes.
#include "cli.h"
#include "dq8/core/elf_headers.h"
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/intake.h"
#include "dq8/core/iso9660.h"
#include "dq8/core/mwo3.h"
#include "dq8/core/region_profile.h"
#include "dq8/core/version.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dq8::cli {

int usage(std::FILE* out) {
    std::fputs(
        "usage: dq8ctl <command> [options]\n"
        "\n"
        "commands:\n"
        "  verify <iso> [--regions DIR] [--json]\n"
        "      identify the disc from its boot file and check it against the region profiles\n"
        "  extract <iso> [--out DIR] [--regions DIR] [--force] [--emit-profile] [--no-verify-hashes] "
        "[--json]\n"
        "      copy the boot file, overlays and IOP modules into DIR/<profile id>/ (default: baserom/)\n"
        "  info <iso | boot elf | overlay .BIN> [--regions DIR] [--json]\n"
        "      describe a disc image (volume, SYSTEM.CNF, listing), an ELF's headers and segments, or an\n"
        "      MWo3 overlay container's header (checked against the region profile that owns its window)\n"
        "  recomp prepare|generate|build|all <iso> [--work DIR] [--maps DIR] [--units LIST] [--jobs N]\n"
        "         [--ps2-recomp PATH] [--module-template DIR] [--abi-include-dir DIR] [--cmake PATH]\n"
        "         [--generator NAME] [--force] [--no-pch] [--debug-info] [--verbose] [--regions DIR] "
        "[--json]\n"
        "      turn the disc's code units into game modules under DIR (default work/recomp): prepare writes\n"
        "      each unit's ELF and recompiler config from the function maps (default DIR/maps), generate\n"
        "      also runs ps2_recomp, build (= all) also compiles each unit through cmake/module into\n"
        "      DIR/modules/<profile>/. Every stage skips work whose inputs did not change.\n"
        "  recomp <stage> --unit-elf NAME=PATH... [--profile-id ID] [options as above]\n"
        "      developer mode: the same stages over given unit ELFs (\"main\" plus overlays) instead of a "
        "disc\n"
        "  make-unit-elf <overlay .BIN> --out FILE [--boot ELF] [--regions DIR] [--json]\n"
        "      write the unit ELF the recompiler reads for one overlay (e_flags from --boot, else 0)\n"
        "  --version | --help\n"
        "\n"
        "--regions defaults to data/regions next to the current directory or the executable's tree.\n"
        "exit status: 0 ok, 1 image or hash mismatch, 2 usage, I/O or tool error\n",
        out);
    return out == stdout ? exit_ok : exit_error;
}

} // namespace dq8::cli

namespace {

using namespace dq8::cli;

void json_mwo3_header(Json& j, const dq8::core::Mwo3Header& h) {
    j.key("header");
    j.begin_object();
    j.member("magic", hex32(h.magic));
    j.member("overlay_id", h.overlay_id);
    j.member("name", h.name);
    j.member("load_address", hex32(h.load_address));
    j.member("text_size", hex32(h.text_size));
    j.member("data_size", hex32(h.data_size));
    j.member("bss_size", hex32(h.bss_size));
    j.member("table_start", hex32(h.table_start));
    j.member("table_end", hex32(h.table_end));
    j.member("table_entries", h.table_entry_count());
    j.end_object();
    j.key("derived");
    j.begin_object();
    j.member("file_size", h.file_size());
    j.member("memsz", hex32(h.memsz()));
    j.member("code_start", hex32(h.code_start()));
    j.member("data_start", hex32(h.data_start()));
    j.member("bss_start", hex32(h.bss_start()));
    j.member("end", hex32(h.end_address()));
    j.end_object();
}

// The one-line summary of a header used by `verify` and `info`.
void print_mwo3_summary(const dq8::core::Mwo3Header& h, std::FILE* out) {
    std::fprintf(out, "id %u  text %s  data %s  bss %s  table %u entries  memsz %s", h.overlay_id,
                 hex32(h.text_size).c_str(), hex32(h.data_size).c_str(), hex32(h.bss_size).c_str(),
                 h.table_entry_count(), hex32(h.memsz()).c_str());
}

void json_verified(Json& j, const dq8::core::VerifiedImage& v) {
    j.key("image");
    j.begin_object();
    j.member("path", v.image_path.string());
    j.member("size", v.image_size);
    j.member("volume_sectors", v.volume_sectors);
    j.end_object();
    j.key("profile");
    j.begin_object();
    j.member("id", v.profile.id);
    j.member("serial", v.profile.serial);
    j.member("title", v.profile.title);
    j.member("region", v.profile.region);
    j.member("video_mode", v.profile.video_mode);
    j.member("source", v.profile.source.string());
    j.end_object();
    j.key("boot");
    j.begin_object();
    j.member("path", v.boot_path);
    j.member("size", v.boot.size);
    j.member("sha1", v.boot.sha1);
    j.member("crc32", v.boot.crc32);
    j.end_object();
    j.key("system_cnf");
    j.begin_object();
    for (const auto& [k, val] : v.system_cnf.entries) {
        j.member(k, val);
    }
    j.end_object();
    j.key("elf");
    j.begin_object();
    j.member("entry", hex32(v.elf.entry));
    j.member("machine", v.elf.machine);
    j.key("load_segments");
    j.begin_array();
    for (const dq8::core::ElfSegment* s : v.elf.load_segments()) {
        j.begin_object();
        j.member("vaddr", hex32(s->vaddr));
        j.member("filesz", hex32(s->filesz));
        j.member("memsz", hex32(s->memsz));
        j.member("flags", s->flags);
        j.end_object();
    }
    j.end_array();
    j.end_object();
    j.key("overlays");
    j.begin_array();
    for (std::size_t i = 0; i < v.profile.overlays.size() && i < v.overlay_headers.size(); ++i) {
        const dq8::core::ProfileOverlay& o = v.profile.overlays[i];
        j.begin_object();
        j.member("name", o.name);
        j.member("path", o.path);
        j.member("index", o.index);
        j.member("segment_index", o.segment_index);
        j.member("segment_memsz", hex32(o.segment_memsz));
        json_mwo3_header(j, v.overlay_headers[i]);
        j.end_object();
    }
    j.end_array();
    j.key("warnings");
    j.begin_array();
    for (const std::string& w : v.warnings) {
        j.value(w);
    }
    j.end_array();
}

void print_verified(const dq8::core::VerifiedImage& v, std::FILE* out) {
    std::fprintf(out, "image:    %s (%llu bytes, %u sectors)\n", v.image_path.string().c_str(),
                 static_cast<unsigned long long>(v.image_size), v.volume_sectors);
    std::fprintf(out, "profile:  %s  %s  (%s, %s)\n", v.profile.id.c_str(), v.profile.title.c_str(),
                 v.profile.region.c_str(), v.profile.video_mode.c_str());
    std::fprintf(out, "boot:     %s  %llu bytes  sha1 %s  crc32 %s\n", v.boot_path.c_str(),
                 static_cast<unsigned long long>(v.boot.size), v.boot.sha1.c_str(), v.boot.crc32.c_str());
    std::string cnf;
    for (const auto& [k, val] : v.system_cnf.entries) {
        cnf += (cnf.empty() ? "" : ", ") + k + " = " + val;
    }
    std::fprintf(out, "SYSTEM.CNF: %s\n", cnf.c_str());
    const auto loads = v.elf.load_segments();
    std::fprintf(out, "ELF:      entry %s, %zu load segments\n", hex32(v.elf.entry).c_str(), loads.size());
    for (std::size_t i = 0; i < v.profile.overlays.size() && i < v.overlay_headers.size(); ++i) {
        const dq8::core::ProfileOverlay& o = v.profile.overlays[i];
        std::fprintf(out, "overlay:  %-8s %-16s ", o.name.c_str(), o.path.c_str());
        print_mwo3_summary(v.overlay_headers[i], out);
        std::fprintf(out, "  (index %u, load segment %u)\n", o.index, o.segment_index);
    }
    print_warnings(v.warnings, out);
}

int run_verify(const Args& args, const char* argv0) {
    const bool json = args.flags.count("json") != 0;
    if (args.positional.size() != 1) {
        std::fputs("dq8ctl verify: expected exactly one <iso> argument\n", stderr);
        return usage(stderr);
    }
    const auto profiles = dq8::core::load_region_profiles(regions_dir(args, argv0));
    const dq8::core::VerifiedImage v = dq8::core::verify_image(args.positional[0], profiles);
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", true);
        json_verified(j, v);
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        print_verified(v, stdout);
        std::printf("OK: the image matches region profile %s (%zu overlay headers checked)\n",
                    v.profile.id.c_str(), v.overlay_headers.size());
    }
    return exit_ok;
}

const char* status_name(dq8::core::FileStatus s) {
    switch (s) {
    case dq8::core::FileStatus::extracted:
        return "extracted";
    case dq8::core::FileStatus::up_to_date:
        return "up-to-date";
    case dq8::core::FileStatus::hash_mismatch:
        return "MISMATCH";
    }
    return "?";
}

int run_extract(const Args& args, const char* argv0) {
    const bool json = args.flags.count("json") != 0;
    if (args.positional.size() != 1) {
        std::fputs("dq8ctl extract: expected exactly one <iso> argument\n", stderr);
        return usage(stderr);
    }
    dq8::core::ExtractOptions options;
    options.force = args.flags.count("force") != 0;
    options.emit_profile = args.flags.count("emit-profile") != 0;
    options.verify_hashes = args.flags.count("no-verify-hashes") == 0;
    const auto out_it = args.values.find("out");
    const fs::path out_root = out_it == args.values.end() ? fs::path("baserom") : fs::path(out_it->second);
    // With --emit-profile the fragment goes to stdout alone, so it can be redirected into a file.
    std::FILE* log = (options.emit_profile && !json) ? stderr : stdout;

    const auto profiles = dq8::core::load_region_profiles(regions_dir(args, argv0));
    const dq8::core::VerifiedImage v = dq8::core::verify_image(args.positional[0], profiles);
    if (!json) {
        print_warnings(v.warnings, log);
        std::fprintf(log, "profile:  %s  ->  %s\n", v.profile.id.c_str(),
                     (out_root / v.profile.id).string().c_str());
    }
    const dq8::core::ExtractReport report = dq8::core::extract(v, out_root, options);
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", !report.has_mismatch());
        j.member("root", report.root.string());
        j.member("profile_dir", report.profile_dir.string());
        j.member("manifest", report.manifest.string());
        json_verified(j, v);
        j.key("files");
        j.begin_array();
        for (const dq8::core::ExtractedFile& f : report.files) {
            j.begin_object();
            j.member("path", f.disc_path);
            j.member("role", f.role);
            j.member("status", status_name(f.status));
            j.member("out", f.out_path.string());
            j.member("size", f.digest.size);
            j.member("sha1", f.digest.sha1);
            j.member("crc32", f.digest.crc32);
            j.member("expected_sha1", f.expected_sha1);
            j.end_object();
        }
        j.end_array();
        j.key("extract_warnings");
        j.begin_array();
        for (const std::string& w : report.warnings) {
            j.value(w);
        }
        j.end_array();
        if (options.emit_profile) {
            j.member("profile_fragment", report.profile_fragment);
        }
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        print_warnings(report.warnings, log);
        for (const dq8::core::ExtractedFile& f : report.files) {
            std::fprintf(log, "%-10s %-24s %10llu bytes  sha1 %s  crc32 %s\n", status_name(f.status),
                         f.disc_path.c_str(), static_cast<unsigned long long>(f.digest.size),
                         f.digest.sha1.c_str(), f.digest.crc32.c_str());
            if (f.status == dq8::core::FileStatus::hash_mismatch) {
                std::fprintf(log, "           expected sha1 %s\n", f.expected_sha1.c_str());
            }
        }
        std::fprintf(log, "%zu extracted, %zu up to date, %zu mismatched; manifest %s\n",
                     report.count(dq8::core::FileStatus::extracted),
                     report.count(dq8::core::FileStatus::up_to_date),
                     report.count(dq8::core::FileStatus::hash_mismatch), report.manifest.string().c_str());
        if (options.emit_profile) {
            std::fputs(report.profile_fragment.c_str(), stdout);
        }
    }
    return report.has_mismatch() ? exit_mismatch : exit_ok;
}

enum class FileKind { iso, elf, mwo3 };

FileKind file_kind(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::uint8_t magic[4] = {};
    in.read(reinterpret_cast<char*>(magic), 4);
    if (in.gcount() != 4) {
        return FileKind::iso;
    }
    if (std::memcmp(magic,
                    "\x7f"
                    "ELF",
                    4) == 0) {
        return FileKind::elf;
    }
    if (dq8::core::looks_like_mwo3(magic)) {
        return FileKind::mwo3;
    }
    return FileKind::iso;
}

dq8::core::ElfHeaders read_elf_file_headers(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open '" + path.string() + "'");
    }
    std::vector<std::uint8_t> head(dq8::core::elf_header_size);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    const std::size_t needed = dq8::core::elf_program_table_end(head);
    head.resize(needed);
    in.clear();
    in.seekg(0);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    if (static_cast<std::size_t>(in.gcount()) != needed) {
        throw Error(ErrorCode::elf_format, "'" + path.string() + "' ends inside its program header table");
    }
    return dq8::core::parse_elf_headers(head);
}

int info_elf(const fs::path& path, bool json) {
    const dq8::core::ElfHeaders h = read_elf_file_headers(path);
    const std::uint64_t size = fs::file_size(path);
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", true);
        j.member("kind", "elf");
        j.member("path", path.string());
        j.member("size", size);
        j.member("type", h.type);
        j.member("machine", h.machine);
        j.member("entry", hex32(h.entry));
        j.member("flags", hex32(h.flags));
        j.member("phnum", h.phnum);
        j.member("shnum", h.shnum);
        j.key("segments");
        j.begin_array();
        for (const dq8::core::ElfSegment& s : h.segments) {
            j.begin_object();
            j.member("type", s.type);
            j.member("offset", hex32(s.offset));
            j.member("vaddr", hex32(s.vaddr));
            j.member("paddr", hex32(s.paddr));
            j.member("filesz", hex32(s.filesz));
            j.member("memsz", hex32(s.memsz));
            j.member("flags", s.flags);
            j.member("align", hex32(s.align));
            j.end_object();
        }
        j.end_array();
        j.end_object();
        std::printf("%s\n", j.str().c_str());
        return exit_ok;
    }
    std::printf("file:     %s (%llu bytes)\n", path.string().c_str(), static_cast<unsigned long long>(size));
    std::printf("ELF32 little-endian, type %u, machine %u%s, flags %s\n", h.type, h.machine,
                h.machine == dq8::core::elf_em_mips ? " (MIPS)" : "", hex32(h.flags).c_str());
    std::printf("entry:    %s\n", hex32(h.entry).c_str());
    std::printf("program headers: %u (table at 0x%x), section headers: %u\n", h.phnum, h.phoff, h.shnum);
    std::printf("  #  type      offset      vaddr       paddr       filesz      memsz       flags  align\n");
    for (std::size_t i = 0; i < h.segments.size(); ++i) {
        const dq8::core::ElfSegment& s = h.segments[i];
        std::printf("  %zu  %-8s  %-10s  %-10s  %-10s  %-10s  %-10s  %c%c%c    %s\n", i,
                    s.type == dq8::core::elf_pt_load ? "LOAD" : hex32(s.type).c_str(),
                    hex32(s.offset).c_str(), hex32(s.vaddr).c_str(), hex32(s.paddr).c_str(),
                    hex32(s.filesz).c_str(), hex32(s.memsz).c_str(),
                    (s.flags & dq8::core::elf_pf_r) ? 'r' : '-', (s.flags & dq8::core::elf_pf_w) ? 'w' : '-',
                    (s.flags & dq8::core::elf_pf_x) ? 'x' : '-', hex32(s.align).c_str());
    }
    return exit_ok;
}

// Describes an overlay container. The window it is checked against comes from the region profile whose
// overlay window starts at the header's load_address, when the profiles can be found; the matching
// profile row (by overlay_id) is compared field by field and the result reported, not enforced: `info`
// describes, `verify` judges.
int info_mwo3(const fs::path& path, bool json, const Args& args, const char* argv0) {
    constexpr std::uint64_t max_size = 16u << 20;
    const std::uint64_t size = fs::file_size(path);
    if (size > max_size) {
        throw Error(ErrorCode::mwo3_size, "'" + path.string() + "' is " + std::to_string(size) +
                                              " bytes, far too large for an overlay container");
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open '" + path.string() + "'");
    }
    std::vector<std::uint8_t> file(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (static_cast<std::uint64_t>(in.gcount()) != size) {
        throw Error(ErrorCode::io_read, "short read on '" + path.string() + "'");
    }

    // A self-consistent parse first (it yields load_address), then the profile whose window starts there.
    const dq8::core::Mwo3Header self = dq8::core::parse_mwo3_header(file, size);
    std::vector<dq8::core::RegionProfile> profiles;
    try {
        profiles = dq8::core::load_region_profiles(regions_dir(args, argv0));
    } catch (const Error&) {
        // No profiles reachable: report the header on its own terms.
    }
    const dq8::core::RegionProfile* profile = nullptr;
    const dq8::core::ProfileOverlay* row = nullptr;
    for (const dq8::core::RegionProfile& p : profiles) {
        if (p.overlay_window.base == self.load_address) {
            profile = &p;
            for (const dq8::core::ProfileOverlay& o : p.overlays) {
                if (o.id == self.overlay_id) {
                    row = &o;
                }
            }
            break;
        }
    }
    std::optional<dq8::core::Mwo3Window> window;
    if (profile != nullptr) {
        window = dq8::core::Mwo3Window{profile->overlay_window.base, profile->overlay_window.end};
    }
    const dq8::core::Mwo3Header h = dq8::core::parse_mwo3_header(file, size, window);
    std::vector<std::string> differences;
    if (row != nullptr) {
        auto differs = [&](const char* what, std::uint32_t seen, std::uint32_t recorded) {
            if (seen != recorded) {
                differences.push_back(std::string(what) + " " + hex32(seen) + " (profile records " +
                                      hex32(recorded) + ")");
            }
        };
        differs("text_size", h.text_size, row->text_size);
        differs("data_size", h.data_size, row->data_size);
        differs("bss_size", h.bss_size, row->bss_size);
        differs("table_entries", h.table_entry_count(), row->table_entries);
        differs("memsz", h.memsz(), row->segment_memsz);
    }

    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", true);
        j.member("kind", "mwo3");
        j.member("path", path.string());
        j.member("size", size);
        json_mwo3_header(j, h);
        j.key("raw_words");
        j.begin_array();
        for (const std::uint32_t w : h.raw_words) {
            j.value(hex32(w));
        }
        j.end_array();
        j.key("profile");
        if (profile == nullptr) {
            j.null();
        } else {
            j.begin_object();
            j.member("id", profile->id);
            j.member("window_base", hex32(profile->overlay_window.base));
            j.member("window_end", hex32(profile->overlay_window.end));
            j.key("overlay");
            if (row == nullptr) {
                j.null();
            } else {
                j.begin_object();
                j.member("name", row->name);
                j.member("path", row->path);
                j.member("index", row->index);
                j.member("segment_index", row->segment_index);
                j.member("matches", differences.empty());
                j.key("differences");
                j.begin_array();
                for (const std::string& d : differences) {
                    j.value(d);
                }
                j.end_array();
                j.end_object();
            }
            j.end_object();
        }
        j.end_object();
        std::printf("%s\n", j.str().c_str());
        return exit_ok;
    }
    std::printf("file:     %s (%llu bytes)\n", path.string().c_str(), static_cast<unsigned long long>(size));
    std::printf("MWo3 overlay container: id %u, name \"%s\"\n", h.overlay_id, h.name.c_str());
    std::printf("header:   load_address %s  text %s  data %s  bss %s  (memsz %s)\n",
                hex32(h.load_address).c_str(), hex32(h.text_size).c_str(), hex32(h.data_size).c_str(),
                hex32(h.bss_size).c_str(), hex32(h.memsz()).c_str());
    std::printf("text:     %s .. %s\n", hex32(h.code_start()).c_str(), hex32(h.data_start()).c_str());
    std::printf("data:     %s .. %s\n", hex32(h.data_start()).c_str(), hex32(h.bss_start()).c_str());
    std::printf("bss:      %s .. %s\n", hex32(h.bss_start()).c_str(), hex32(h.end_address()).c_str());
    std::printf("table:    %s .. %s  (%u constructor entries, in the data range)\n",
                hex32(h.table_start).c_str(), hex32(h.table_end).c_str(), h.table_entry_count());
    if (profile == nullptr) {
        std::printf("profile:  none has an overlay window at %s; header checked for self-consistency only\n",
                    hex32(h.load_address).c_str());
    } else if (row == nullptr) {
        std::printf("profile:  %s owns the window %s..%s but lists no overlay with id %u\n",
                    profile->id.c_str(), hex32(profile->overlay_window.base).c_str(),
                    hex32(profile->overlay_window.end).c_str(), h.overlay_id);
    } else if (differences.empty()) {
        std::printf(
            "profile:  %s overlay %s (%s, index %u, load segment %u): the header matches the profile\n",
            profile->id.c_str(), row->name.c_str(), row->path.c_str(), row->index, row->segment_index);
    } else {
        std::printf("profile:  %s overlay %s (%s): the header DIFFERS from the profile\n",
                    profile->id.c_str(), row->name.c_str(), row->path.c_str());
        for (const std::string& d : differences) {
            std::printf("          %s\n", d.c_str());
        }
    }
    std::printf("raw header words:\n");
    for (std::size_t i = 0; i < h.raw_words.size(); i += 4) {
        std::printf("  +0x%02zx  %s %s %s %s\n", i * 4, hex32(h.raw_words[i]).c_str(),
                    hex32(h.raw_words[i + 1]).c_str(), hex32(h.raw_words[i + 2]).c_str(),
                    hex32(h.raw_words[i + 3]).c_str());
    }
    return exit_ok;
}

int info_iso(const fs::path& path, bool json) {
    const dq8::core::iso::Image image = dq8::core::iso::Image::open(path);
    const dq8::core::iso::VolumeInfo& vol = image.volume();
    std::string cnf_error;
    dq8::core::SystemCnf cnf;
    bool have_cnf = false;
    if (const dq8::core::iso::Entry* e = image.find("SYSTEM.CNF")) {
        try {
            const auto bytes = image.read_all(*e, 64u << 10);
            cnf = dq8::core::parse_system_cnf(
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            have_cnf = true;
        } catch (const Error& err) {
            cnf_error = err.what();
        }
    } else {
        cnf_error = "no SYSTEM.CNF on the disc";
    }
    std::size_t directories = 0;
    std::size_t files = 0;
    for (const dq8::core::iso::Entry& e : image.entries()) {
        (e.is_directory() ? directories : files) += 1;
    }
    if (json) {
        Json j;
        j.begin_object();
        j.member("ok", true);
        j.member("kind", "iso9660");
        j.member("path", path.string());
        j.member("size", image.image_size());
        j.member("sectors", image.image_sectors());
        j.key("volume");
        j.begin_object();
        j.member("system_identifier", vol.system_identifier);
        j.member("volume_identifier", vol.volume_identifier);
        j.member("application_identifier", vol.application_identifier);
        j.member("volume_space_size", vol.volume_space_size);
        j.member("logical_block_size", static_cast<std::uint64_t>(vol.logical_block_size));
        j.member("path_table_size", vol.path_table_size);
        j.member("path_table_lba_l", vol.path_table_lba_l);
        j.member("path_table_lba_m", vol.path_table_lba_m);
        j.member("root_lba", vol.root_lba);
        j.member("root_size", vol.root_size);
        j.member("descriptors", vol.descriptor_count);
        j.member("has_boot_record", vol.has_boot_record);
        j.member("has_supplementary", vol.has_supplementary);
        j.end_object();
        j.key("path_table");
        j.begin_object();
        j.member("directories", vol.path_table_directories);
        j.member("tree_directories", static_cast<std::uint64_t>(directories));
        j.member("consistent", true);
        j.end_object();
        j.key("system_cnf");
        if (have_cnf) {
            j.begin_object();
            for (const auto& [k, v] : cnf.entries) {
                j.member(k, v);
            }
            j.end_object();
        } else {
            j.value(cnf_error);
        }
        j.key("entries");
        j.begin_array();
        for (const dq8::core::iso::Entry& e : image.entries()) {
            j.begin_object();
            j.member("path", e.path);
            j.member("identifier", e.identifier);
            j.member("lba", e.lba);
            j.member("size", e.size);
            j.member("directory", e.is_directory());
            j.end_object();
        }
        j.end_array();
        j.end_object();
        std::printf("%s\n", j.str().c_str());
        return exit_ok;
    }
    std::printf("image:    %s\n", path.string().c_str());
    std::printf("size:     %llu bytes, %u sectors of %u (volume declares %u)\n",
                static_cast<unsigned long long>(image.image_size()), image.image_sectors(),
                static_cast<unsigned>(vol.logical_block_size), vol.volume_space_size);
    std::printf("system:   '%s'  volume: '%s'  application: '%s'\n", vol.system_identifier.c_str(),
                vol.volume_identifier.c_str(), vol.application_identifier.c_str());
    std::printf("descriptors: %u%s%s\n", vol.descriptor_count, vol.has_boot_record ? ", boot record" : "",
                vol.has_supplementary ? ", supplementary (Joliet, ignored)" : "");
    std::printf("path table: %u bytes at LBA %u (L) and %u (M); %u directories, matches the directory tree\n",
                vol.path_table_size, vol.path_table_lba_l, vol.path_table_lba_m, vol.path_table_directories);
    std::printf("root:     LBA %u, %u bytes; %zu directories, %zu files\n", vol.root_lba, vol.root_size,
                directories, files);
    if (have_cnf) {
        std::string line;
        for (const auto& [k, v] : cnf.entries) {
            line += (line.empty() ? "" : ", ") + k + " = " + v;
        }
        std::printf("SYSTEM.CNF: %s\n", line.c_str());
    } else {
        std::printf("SYSTEM.CNF: %s\n", cnf_error.c_str());
    }
    std::printf("\n      LBA         size  path\n");
    for (const dq8::core::iso::Entry& e : image.entries()) {
        std::printf("%9u %12u  %s%s\n", e.lba, e.size, e.path.c_str(),
                    (e.is_directory() && e.path != "/") ? "/" : "");
    }
    return exit_ok;
}

int run_info(const Args& args, const char* argv0) {
    const bool json = args.flags.count("json") != 0;
    if (args.positional.size() != 1) {
        std::fputs("dq8ctl info: expected exactly one <path> argument\n", stderr);
        return usage(stderr);
    }
    const fs::path path = args.positional[0];
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        throw Error(ErrorCode::io_open, "'" + path.string() + "' is not a file");
    }
    switch (file_kind(path)) {
    case FileKind::elf:
        return info_elf(path, json);
    case FileKind::mwo3:
        return info_mwo3(path, json, args, argv0);
    case FileKind::iso:
        break;
    }
    return info_iso(path, json);
}

int report_error(const Args& args, const Error& e) {
    const int status = dq8::core::is_mismatch(e.code()) ? exit_mismatch : exit_error;
    if (args.flags.count("json") != 0) {
        Json j;
        j.begin_object();
        j.member("ok", false);
        j.key("error");
        j.begin_object();
        j.member("code", dq8::core::to_string(e.code()));
        j.member("message", e.what());
        j.end_object();
        j.end_object();
        std::printf("%s\n", j.str().c_str());
    } else {
        std::fprintf(stderr, "dq8ctl %s: %s [%s]\n", args.command.c_str(), e.what(),
                     std::string(dq8::core::to_string(e.code())).c_str());
    }
    return status;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage(stderr);
    }
    const char* command = argv[1];
    if (std::strcmp(command, "--version") == 0 || std::strcmp(command, "-V") == 0) {
        std::printf("dq8ctl %s\n", dq8::core::version());
        return exit_ok;
    }
    if (std::strcmp(command, "--help") == 0 || std::strcmp(command, "-h") == 0 ||
        std::strcmp(command, "help") == 0) {
        return usage(stdout);
    }
    Args args;
    std::string error;
    if (!parse_args(argc, argv, args, error)) {
        std::fprintf(stderr, "dq8ctl: %s\n", error.c_str());
        return usage(stderr);
    }
    try {
        if (args.command == "verify") {
            return run_verify(args, argv[0]);
        }
        if (args.command == "extract") {
            return run_extract(args, argv[0]);
        }
        if (args.command == "info") {
            return run_info(args, argv[0]);
        }
        if (args.command == "recomp") {
            return run_recomp(args, argv[0]);
        }
        if (args.command == "make-unit-elf") {
            return run_make_unit_elf(args, argv[0]);
        }
    } catch (const Error& e) {
        return report_error(args, e);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dq8ctl %s: %s\n", args.command.c_str(), e.what());
        return exit_error;
    }
    std::fprintf(stderr, "dq8ctl: unknown command '%s'\n", command);
    return usage(stderr);
}
