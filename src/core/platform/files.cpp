// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "platform/files.h"

#include "dq8/core/error.h"
#include "dq8/core/hash.h"

#include <algorithm>
#include <fstream>

namespace dq8::core::platform {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t chunk_size = 1u << 20;

std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace

std::vector<std::uint8_t> read_file(const fs::path& file, std::uint64_t max_bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open '" + file.string() + "'");
    }
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        throw Error(ErrorCode::io_read, "cannot size '" + file.string() + "': " + ec.message());
    }
    if (size > max_bytes) {
        throw Error(ErrorCode::io_read, "'" + file.string() + "' is " + std::to_string(size) +
                                            " bytes, more than the " + std::to_string(max_bytes) +
                                            " this reader accepts");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) {
        throw Error(ErrorCode::io_read, "short read on '" + file.string() + "'");
    }
    return bytes;
}

std::string read_text_file(const fs::path& file, std::uint64_t max_bytes) {
    const std::vector<std::uint8_t> bytes = read_file(file, max_bytes);
    return std::string(bytes.begin(), bytes.end());
}

void make_directories(const fs::path& dir) {
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw Error(ErrorCode::io_write, "cannot create '" + dir.string() + "': " + ec.message());
    }
}

void write_file_atomic(const fs::path& file, std::span<const std::uint8_t> bytes) {
    make_directories(file.parent_path());
    const fs::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw Error(ErrorCode::io_write, "cannot create '" + tmp.string() + "'");
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            throw Error(ErrorCode::io_write, "write error on '" + tmp.string() + "'");
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        // Windows refuses to rename over an existing file in some configurations.
        ec.clear();
        fs::remove(file, ec);
        ec.clear();
        fs::rename(tmp, file, ec);
    }
    if (ec) {
        throw Error(ErrorCode::io_write, "cannot replace '" + file.string() + "': " + ec.message());
    }
}

void write_file_atomic(const fs::path& file, std::string_view text) {
    write_file_atomic(file, as_bytes(text));
}

bool write_file_if_changed(const fs::path& file, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) == bytes.size() && !ec) {
        std::ifstream in(file, std::ios::binary);
        std::vector<char> chunk(std::min<std::size_t>(chunk_size, std::max<std::size_t>(bytes.size(), 1)));
        std::size_t done = 0;
        bool same = static_cast<bool>(in);
        while (same && done < bytes.size()) {
            const std::size_t n = std::min(chunk.size(), bytes.size() - done);
            in.read(chunk.data(), static_cast<std::streamsize>(n));
            same = static_cast<std::size_t>(in.gcount()) == n &&
                   std::equal(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n),
                              reinterpret_cast<const char*>(bytes.data() + done));
            done += n;
        }
        if (same) {
            return false;
        }
    }
    write_file_atomic(file, bytes);
    return true;
}

bool write_file_if_changed(const fs::path& file, std::string_view text) {
    return write_file_if_changed(file, as_bytes(text));
}

std::string sha1_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open '" + file.string() + "'");
    }
    Sha1 sha;
    std::vector<std::uint8_t> chunk(chunk_size);
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) {
            break;
        }
        sha.update(std::span<const std::uint8_t>(chunk.data(), n));
    }
    if (in.bad()) {
        throw Error(ErrorCode::io_read, "read error on '" + file.string() + "'");
    }
    return to_hex(sha.finish());
}

bool same_contents(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const std::uintmax_t size_a = fs::file_size(a, ec);
    if (ec) {
        return false;
    }
    const std::uintmax_t size_b = fs::file_size(b, ec);
    if (ec || size_a != size_b) {
        return false;
    }
    std::ifstream in_a(a, std::ios::binary);
    std::ifstream in_b(b, std::ios::binary);
    if (!in_a || !in_b) {
        return false;
    }
    std::vector<char> chunk_a(chunk_size);
    std::vector<char> chunk_b(chunk_size);
    while (true) {
        in_a.read(chunk_a.data(), static_cast<std::streamsize>(chunk_a.size()));
        in_b.read(chunk_b.data(), static_cast<std::streamsize>(chunk_b.size()));
        const std::streamsize n = in_a.gcount();
        if (n != in_b.gcount()) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (!std::equal(chunk_a.begin(), chunk_a.begin() + n, chunk_b.begin())) {
            return false;
        }
    }
}

} // namespace dq8::core::platform
