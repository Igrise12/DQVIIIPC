// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// File helpers shared by the setup-pipeline stages (internal to libdq8core). Every failure is an Error with
// io_open, io_read or io_write and the path in the message.
namespace dq8::core::platform {

// Reads a whole file, refusing (io_read) anything larger than max_bytes.
std::vector<std::uint8_t> read_file(const std::filesystem::path& file, std::uint64_t max_bytes = 256u << 20);
std::string read_text_file(const std::filesystem::path& file, std::uint64_t max_bytes = 256u << 20);

// Writes `bytes` to a temporary file next to `file` and renames it into place, creating parent directories.
void write_file_atomic(const std::filesystem::path& file, std::span<const std::uint8_t> bytes);
void write_file_atomic(const std::filesystem::path& file, std::string_view text);

// Like write_file_atomic(), but leaves the file (and its modification time) alone when it already holds
// exactly these bytes. Returns true when the file was written.
bool write_file_if_changed(const std::filesystem::path& file, std::span<const std::uint8_t> bytes);
bool write_file_if_changed(const std::filesystem::path& file, std::string_view text);

// SHA-1 of a file's contents, lowercase hex, streamed in 1 MiB chunks.
std::string sha1_file(const std::filesystem::path& file);

// True when both files exist and hold the same bytes.
bool same_contents(const std::filesystem::path& a, const std::filesystem::path& b);

void make_directories(const std::filesystem::path& dir);

} // namespace dq8::core::platform
