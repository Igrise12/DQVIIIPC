// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"

namespace dq8::core {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::io_open:
        return "io-open";
    case ErrorCode::io_read:
        return "io-read";
    case ErrorCode::io_write:
        return "io-write";
    case ErrorCode::iso_descriptor:
        return "iso-descriptor";
    case ErrorCode::iso_truncated:
        return "iso-truncated";
    case ErrorCode::iso_endian_mismatch:
        return "iso-endian-mismatch";
    case ErrorCode::iso_record:
        return "iso-record";
    case ErrorCode::iso_unsupported:
        return "iso-unsupported";
    case ErrorCode::iso_cycle:
        return "iso-cycle";
    case ErrorCode::iso_depth:
        return "iso-depth";
    case ErrorCode::iso_path_table:
        return "iso-path-table";
    case ErrorCode::iso_not_found:
        return "iso-not-found";
    case ErrorCode::iso_range:
        return "iso-range";
    case ErrorCode::elf_format:
        return "elf-format";
    case ErrorCode::profile_parse:
        return "profile-parse";
    case ErrorCode::profile_schema:
        return "profile-schema";
    case ErrorCode::profile_conflict:
        return "profile-conflict";
    case ErrorCode::system_cnf:
        return "system-cnf";
    case ErrorCode::boot_file_missing:
        return "boot-file-missing";
    case ErrorCode::unknown_image:
        return "unknown-image";
    case ErrorCode::image_mismatch:
        return "image-mismatch";
    case ErrorCode::extract_hash_mismatch:
        return "extract-hash-mismatch";
    }
    return "unknown";
}

bool is_mismatch(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::system_cnf:
    case ErrorCode::boot_file_missing:
    case ErrorCode::unknown_image:
    case ErrorCode::image_mismatch:
    case ErrorCode::extract_hash_mismatch:
        return true;
    default:
        return false;
    }
}

} // namespace dq8::core
