// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/hash.h"
#include "dq8/core/intake.h"
#include "dq8/core/version.h"

#include <algorithm>
#include <fstream>
#include <toml.hpp>
#include <vector>

namespace dq8::core {

namespace {

constexpr std::uint64_t chunk_size = 1u << 20;
constexpr int manifest_schema_version = 1;

struct ManifestRecord {
    std::string path;
    std::string role;
    std::uint64_t size = 0;
    std::string sha1;
    std::string crc32;
};

struct Manifest {
    std::string profile;
    std::uint64_t image_size = 0;
    std::string boot_sha1;
    std::vector<ManifestRecord> files;

    const ManifestRecord* find(const std::string& path) const {
        for (const ManifestRecord& r : files) {
            if (r.path == path) {
                return &r;
            }
        }
        return nullptr;
    }

    void upsert(ManifestRecord record) {
        for (ManifestRecord& r : files) {
            if (r.path == record.path) {
                r = std::move(record);
                return;
            }
        }
        files.push_back(std::move(record));
    }
};

std::string toml_quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Reads an existing manifest. A missing file yields an empty manifest; a malformed one is reported through
// `warning` and treated as empty, so a damaged record only costs a re-extraction.
Manifest read_manifest(const std::filesystem::path& file, std::string& warning) {
    Manifest m;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return m;
    }
    auto parsed = toml::try_parse(in, file.string());
    if (parsed.is_err()) {
        warning = "ignoring malformed manifest '" + file.string() + "'";
        return m;
    }
    try {
        const toml::value root = std::move(parsed.unwrap());
        if (toml::find_or<int>(root, "schema_version", 0) != manifest_schema_version) {
            warning = "ignoring manifest '" + file.string() + "' with an unknown schema version";
            return m;
        }
        m.profile = toml::find_or<std::string>(root, "profile", "");
        m.image_size = static_cast<std::uint64_t>(toml::find_or<std::int64_t>(root, "image_size", 0));
        m.boot_sha1 = toml::find_or<std::string>(root, "boot_sha1", "");
        if (root.contains("files") && root.at("files").is_array()) {
            for (const toml::value& t : root.at("files").as_array()) {
                ManifestRecord r;
                r.path = toml::find_or<std::string>(t, "path", "");
                r.role = toml::find_or<std::string>(t, "role", "");
                r.size = static_cast<std::uint64_t>(toml::find_or<std::int64_t>(t, "size", 0));
                r.sha1 = normalize_hex(toml::find_or<std::string>(t, "sha1", ""));
                r.crc32 = normalize_hex(toml::find_or<std::string>(t, "crc32", ""));
                if (!r.path.empty()) {
                    m.files.push_back(std::move(r));
                }
            }
        }
    } catch (const toml::exception&) {
        warning = "ignoring malformed manifest '" + file.string() + "'";
        return Manifest{};
    }
    return m;
}

void write_text_file(const std::filesystem::path& file, const std::string& text) {
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw Error(ErrorCode::io_write, "cannot create '" + tmp.string() + "'");
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            throw Error(ErrorCode::io_write, "write error on '" + tmp.string() + "'");
        }
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        throw Error(ErrorCode::io_write, "cannot replace '" + file.string() + "': " + ec.message());
    }
}

void write_manifest(const std::filesystem::path& file, const Manifest& m) {
    std::string text;
    text +=
        "# Written by dq8ctl extract. Records what was extracted from your disc so that re-running extract\n";
    text +=
        "# can skip files that are already present. Everything in this directory is game-derived: never\n";
    text += "# commit it (baserom/.gitignore excludes it).\n";
    text += "schema_version = " + std::to_string(manifest_schema_version) + "\n";
    text += "profile = " + toml_quote(m.profile) + "\n";
    text += "tool_version = " + toml_quote(version()) + "\n";
    text += "image_size = " + std::to_string(m.image_size) + "\n";
    text += "boot_sha1 = " + toml_quote(m.boot_sha1) + "\n";
    for (const ManifestRecord& r : m.files) {
        text += "\n[[files]]\n";
        text += "path = " + toml_quote(r.path) + "\n";
        text += "role = " + toml_quote(r.role) + "\n";
        text += "size = " + std::to_string(r.size) + "\n";
        text += "sha1 = " + toml_quote(r.sha1) + "\n";
        text += "crc32 = " + toml_quote(r.crc32) + "\n";
    }
    write_text_file(file, text);
}

void ensure_gitignore(const std::filesystem::path& root) {
    const std::filesystem::path file = root / ".gitignore";
    const std::string wanted = "*\n";
    std::ifstream in(file, std::ios::binary);
    if (in) {
        const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (existing == wanted) {
            return;
        }
    }
    write_text_file(file, wanted);
}

struct Target {
    std::string disc_path;
    std::string role;
    std::uint64_t expected_size = 0;
    std::string expected_sha1;
};

std::vector<Target> targets_of(const RegionProfile& p) {
    std::vector<Target> targets;
    targets.push_back({p.boot.path, "boot", p.boot.size, p.boot.sha1});
    for (const ProfileOverlay& o : p.overlays) {
        targets.push_back({o.path, "overlay", o.size, o.sha1});
    }
    for (const ProfileFile& f : p.files) {
        targets.push_back({f.path, f.role, f.size, f.sha1});
    }
    return targets;
}

FileDigest copy_entry(const iso::Image& image, const iso::Entry& entry, const std::filesystem::path& out,
                      const ProgressFn& progress) {
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (ec) {
        throw Error(ErrorCode::io_write,
                    "cannot create '" + out.parent_path().string() + "': " + ec.message());
    }
    const std::filesystem::path part = out.string() + ".part";
    Sha1 sha;
    Crc32 crc;
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw Error(ErrorCode::io_write, "cannot create '" + part.string() + "'");
        }
        std::vector<std::uint8_t> chunk(
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, entry.size)));
        std::uint64_t done = 0;
        while (done < entry.size) {
            const std::uint64_t n = std::min<std::uint64_t>(chunk_size, entry.size - done);
            const std::span<std::uint8_t> piece(chunk.data(), static_cast<std::size_t>(n));
            image.read(entry, done, piece);
            sha.update(piece);
            crc.update(piece);
            file.write(reinterpret_cast<const char*>(piece.data()),
                       static_cast<std::streamsize>(piece.size()));
            if (!file) {
                throw Error(ErrorCode::io_write, "write error on '" + part.string() + "'");
            }
            done += n;
            if (progress) {
                progress(entry.path, done, entry.size);
            }
        }
    }
    std::filesystem::remove(out, ec);
    ec.clear();
    std::filesystem::rename(part, out, ec);
    if (ec) {
        throw Error(ErrorCode::io_write, "cannot move '" + part.string() + "' into place: " + ec.message());
    }
    FileDigest d;
    d.size = entry.size;
    d.sha1 = to_hex(sha.finish());
    d.crc32 = to_hex(crc.value());
    return d;
}

std::string hex_upper(std::uint32_t v) {
    std::string s = to_hex(v);
    for (char& c : s) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const std::size_t first = s.find_first_not_of('0');
    return "0x" + (first == std::string::npos ? std::string("0") : s.substr(first));
}

} // namespace

std::size_t ExtractReport::count(FileStatus status) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        files.begin(), files.end(), [status](const ExtractedFile& f) { return f.status == status; }));
}

ExtractReport extract(const VerifiedImage& verified, const std::filesystem::path& out_root,
                      const ExtractOptions& options) {
    const RegionProfile& profile = verified.profile;
    const iso::Image image = iso::Image::open(verified.image_path);

    ExtractReport report;
    report.root = out_root;
    report.profile_dir = out_root / profile.id;
    report.manifest = report.profile_dir / "manifest.toml";
    std::error_code ec;
    std::filesystem::create_directories(report.profile_dir, ec);
    if (ec) {
        throw Error(ErrorCode::io_write,
                    "cannot create '" + report.profile_dir.string() + "': " + ec.message());
    }
    ensure_gitignore(out_root);

    std::string manifest_warning;
    Manifest manifest = read_manifest(report.manifest, manifest_warning);
    if (!manifest_warning.empty()) {
        report.warnings.push_back(manifest_warning);
    }
    if (!manifest.profile.empty() && manifest.profile != profile.id) {
        report.warnings.push_back("manifest was written for profile '" + manifest.profile +
                                  "'; starting over");
        manifest = Manifest{};
    }
    manifest.profile = profile.id;
    manifest.image_size = verified.image_size;
    manifest.boot_sha1 = verified.boot.sha1;

    for (const Target& t : targets_of(profile)) {
        const iso::Entry& entry = image.require(t.disc_path);
        ExtractedFile f;
        f.disc_path = t.disc_path;
        f.role = t.role;
        f.out_path =
            report.profile_dir / std::filesystem::path(t.disc_path, std::filesystem::path::generic_format);
        f.expected_sha1 = t.expected_sha1;

        bool reuse = false;
        if (!options.force) {
            const ManifestRecord* rec = manifest.find(t.disc_path);
            if (rec != nullptr && rec->size == entry.size &&
                std::filesystem::is_regular_file(f.out_path, ec) &&
                std::filesystem::file_size(f.out_path, ec) == rec->size) {
                const FileDigest on_disk = digest_file(f.out_path);
                const bool matches_profile =
                    t.expected_sha1.empty() || !options.verify_hashes || on_disk.sha1 == t.expected_sha1;
                if (on_disk.sha1 == rec->sha1 && matches_profile) {
                    f.digest = on_disk;
                    f.status = FileStatus::up_to_date;
                    reuse = true;
                }
            }
        }
        if (!reuse) {
            f.digest = copy_entry(image, entry, f.out_path, options.progress);
            const bool mismatch =
                options.verify_hashes && !t.expected_sha1.empty() && f.digest.sha1 != t.expected_sha1;
            f.status = mismatch ? FileStatus::hash_mismatch : FileStatus::extracted;
            manifest.upsert({t.disc_path, t.role, f.digest.size, f.digest.sha1, f.digest.crc32});
            write_manifest(report.manifest, manifest);
        }
        report.files.push_back(std::move(f));
    }
    if (options.emit_profile) {
        report.profile_fragment = format_profile_fragment(verified, report);
    }
    return report;
}

std::string format_profile_fragment(const VerifiedImage& verified, const ExtractReport& report) {
    const RegionProfile& p = verified.profile;
    auto digest_for = [&](const std::string& disc_path) -> const FileDigest* {
        for (const ExtractedFile& f : report.files) {
            if (f.disc_path == disc_path) {
                return &f.digest;
            }
        }
        return nullptr;
    };
    auto hashes = [&](const std::string& disc_path) {
        const FileDigest* d = digest_for(disc_path);
        if (d == nullptr) {
            return std::string("size = 0\nsha1 = \"\"\ncrc32 = \"\"\n");
        }
        return "size = " + std::to_string(d->size) + "\nsha1 = \"" + d->sha1 + "\"\ncrc32 = \"" + d->crc32 +
               "\"\n";
    };
    std::string out;
    out += "# Emitted by `dq8ctl extract --emit-profile` for profile " + p.id + ". Paste over the matching\n";
    out += "# rows of data/regions/" + p.id + ".toml. Names, sizes and hashes only.\n\n";
    out += "[boot]\n";
    out += "path = " + toml_quote(p.boot.path) + "\n";
    out += hashes(p.boot.path);
    out += "entry = " + hex_upper(p.boot.entry) + "\n";
    out += "load_address = " + hex_upper(p.boot.load_address) + "\n";
    out += "main_memsz = " + hex_upper(p.boot.main_memsz) + "\n";
    for (const ProfileOverlay& o : p.overlays) {
        out += "\n[[overlays]]\n";
        out += "name = " + toml_quote(o.name) + "\n";
        out += "id = " + std::to_string(o.id) + "\n";
        out += "path = " + toml_quote(o.path) + "\n";
        out += "segment_memsz = " + hex_upper(o.segment_memsz) + "\n";
        out += hashes(o.path);
    }
    for (const ProfileFile& f : p.files) {
        out += "\n[[files]]\n";
        out += "path = " + toml_quote(f.path) + "\n";
        out += "role = " + toml_quote(f.role) + "\n";
        out += hashes(f.path);
    }
    return out;
}

} // namespace dq8::core
