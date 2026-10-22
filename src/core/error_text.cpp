// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error_text.h"

namespace dq8::core {

ErrorText error_text(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::io_open:
        return {"A file could not be opened.", "Check that the file exists and that you may read it."};
    case ErrorCode::io_read:
        return {"A file could not be read completely.",
                "Check the disc image or the drive it is on, then try again."};
    case ErrorCode::io_write:
        return {"A file could not be written.", "Check the free space and the permissions of the folder."};
    case ErrorCode::iso_descriptor:
    case ErrorCode::iso_truncated:
    case ErrorCode::iso_endian_mismatch:
    case ErrorCode::iso_record:
    case ErrorCode::iso_unsupported:
    case ErrorCode::iso_cycle:
    case ErrorCode::iso_depth:
    case ErrorCode::iso_path_table:
    case ErrorCode::iso_not_found:
    case ErrorCode::iso_range:
        return {"The file is not a readable PS2 disc image.",
                "Make a fresh image (.iso) of the original disc and choose it."};
    case ErrorCode::elf_format:
    case ErrorCode::mwo3_magic:
    case ErrorCode::mwo3_size:
    case ErrorCode::mwo3_id:
    case ErrorCode::mwo3_range:
    case ErrorCode::mwo3_table:
    case ErrorCode::mwo3_name:
        return {"A program file on the disc image is damaged or modified.",
                "Make a fresh image of the original disc and choose it."};
    case ErrorCode::profile_parse:
    case ErrorCode::profile_schema:
    case ErrorCode::profile_conflict:
        return {"The program's region data is damaged.", "Reinstall DQ8-Native."};
    case ErrorCode::system_cnf:
    case ErrorCode::boot_file_missing:
        return {"The disc image is not a PS2 game disc.", "Choose an image of the Dragon Quest VIII disc."};
    case ErrorCode::unknown_image:
        return {"This disc image is not a supported version of Dragon Quest VIII.",
                "Use an image of the original North American (NTSC-U) disc, without patches."};
    case ErrorCode::image_mismatch:
    case ErrorCode::extract_hash_mismatch:
        return {"The disc image differs from the original disc.",
                "Make a fresh image of the original disc; patched or damaged images are not supported."};
    case ErrorCode::elf_layout:
    case ErrorCode::map_parse:
    case ErrorCode::map_invalid:
    case ErrorCode::recomp_failed:
        return {"Translating the game's code failed.", "Export the diagnostics and report the problem."};
    case ErrorCode::module_build_failed:
        return {
            "Compiling the game failed.",
            "Check the free memory and disk space, then run the setup again; if it fails again, export the "
            "diagnostics and report the problem."};
    case ErrorCode::process_spawn:
        return {"A helper program could not be started.", "Reinstall DQ8-Native, then run the setup again."};
    case ErrorCode::process_timeout:
        return {"A helper program stopped responding.", "Run the setup again."};
    case ErrorCode::process_cancelled:
    case ErrorCode::setup_cancelled:
        return {"The setup was cancelled.", "Run the setup again to continue where it stopped."};
    case ErrorCode::tool_missing:
    case ErrorCode::toolchain_incomplete:
        return {
            "A tool the setup needs (a C++ compiler, CMake or Ninja) is missing.",
            "Install the tools the setup lists, or let it download its own toolchain, then run it again."};
    case ErrorCode::work_stale:
        return {"The setup's working files belong to another disc or version.",
                "Run the setup again; it rebuilds what is out of date."};
    case ErrorCode::invalid_argument:
        return {"An option has a value that does not exist.", "Check the option and try again."};
    case ErrorCode::settings_parse:
        return {"A settings file is damaged.",
                "Reset the settings in the launcher, or delete the file named."};
    case ErrorCode::settings_invalid:
        return {"A setting has a value that is not allowed.", "Choose a value from the list or range shown."};
    case ErrorCode::download_failed:
        return {"The toolchain download failed or arrived damaged.",
                "Check the internet connection and try again."};
    case ErrorCode::insufficient_space:
        return {"There is not enough free disk space for the setup.",
                "Free some space (the setup needs about 2 GB while building), then run it again."};
    case ErrorCode::smoke_failed:
        return {"The game was built but did not start correctly.",
                "Export the diagnostics and report the problem."};
    case ErrorCode::maps_missing:
        return {"The program has no code maps for this disc.", "Reinstall DQ8-Native or update it."};
    case ErrorCode::registry_parse:
        return {"The list of installed games is damaged.", "Run the setup again; it rewrites the list."};
    }
    return {"An unexpected error occurred.", "Export the diagnostics and report the problem."};
}

} // namespace dq8::core
