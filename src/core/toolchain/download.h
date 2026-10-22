// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The bundled Windows toolchain (ADR-0018): the pinned llvm-mingw release, downloaded only after the user
// agreed, checked against its pinned SHA-256 and unpacked into <bundled_root>/<version>/ (bundled_root
// defaults to platform::user_data_dir() / "toolchain", toolchain::DetectOptions::bundled_root). M5 session E.
//
//   <bundled_root>/downloads/<archive>.part   while downloading; a later call resumes from its size
//   <bundled_root>/<version>.unpack/          while unpacking (removed on the next attempt)
//   <bundled_root>/<version>/                 the toolchain: bin/x86_64-w64-mingw32-clang++.exe, ...
//   <bundled_root>/<version>/.dq8-toolchain   written last: "<version> <sha256>"; its presence means complete
//
// Transport: WinHTTP on Windows; the curl program on other systems (contributors and tests; Linux users build
// with the system compiler). file:// URLs are copied in-process on every platform. The zip is unpacked by
// `cmake -E tar` (cmake ships with the program, packaging/windows/), so libdq8core needs no archive library.
namespace dq8::core::toolchain {

struct DownloadPin {
    std::string version;      // the release, also the directory name: "20261020"
    std::string url;          // http(s):// or file://
    std::string sha256;       // 64 lowercase hex digits
    std::uint64_t size = 0;   // the archive's size in bytes; 0 when unknown (progress then has no total)
    std::string archive_root; // the single top-level directory inside the archive, moved to <version>/
    std::uint64_t unpacked_bytes = 0; // the space the unpacked tree needs, for the free-space check
    std::uint64_t entries = 0; // files and directories in the archive (unpack progress); 0 when unknown
};

// The pinned Windows-hosted llvm-mingw (cmake/toolchains/llvm-mingw-pin.cmake holds the same pin; a unit test
// compares them).
const DownloadPin& pinned_llvm_mingw();

enum class DownloadPhase { download, verify, unpack };

struct DownloadProgress {
    DownloadPhase phase = DownloadPhase::download;
    std::uint64_t done = 0;  // bytes (download, verify) or files (unpack)
    std::uint64_t total = 0; // 0 when unknown
};

struct DownloadOptions {
    // The user's consent. Without it nothing is downloaded and the call fails with download-failed; an
    // already installed toolchain is still returned.
    bool consent = false;
    std::filesystem::path bundled_root; // empty: platform::user_data_dir() / "toolchain"
    DownloadPin pin = pinned_llvm_mingw();
    std::filesystem::path cmake = "cmake"; // unpacks the archive: a path, or a name looked up in PATH
    std::filesystem::path curl = "curl";   // the transport on systems other than Windows
    std::function<void(const DownloadProgress&)> on_progress;
    const std::atomic<bool>* cancel = nullptr; // set to stop; the next call resumes the download
    std::chrono::seconds stall_timeout{60};    // no data for this long ends the attempt (download-failed)
    bool keep_archive = false;                 // keep downloads/<archive> after unpacking (default: delete)
};

struct DownloadResult {
    std::filesystem::path root;         // <bundled_root>/<version>
    bool already_installed = false;     // nothing was downloaded or unpacked
    std::uint64_t downloaded_bytes = 0; // transferred by this call
    std::uint64_t resumed_from = 0;     // the partial download's size this call continued from
};

// The installed toolchain for `pin` under `bundled_root` (the marker file names the pin's version and
// digest), or nullopt. Never throws.
std::optional<std::filesystem::path> installed_toolchain(const std::filesystem::path& bundled_root,
                                                         const DownloadPin& pin = pinned_llvm_mingw());

// Downloads, verifies and unpacks the toolchain unless it is already installed. Throws Error(download_failed)
// (no consent, network or HTTP failure, checksum mismatch, a malformed archive), Error(insufficient_space),
// Error(setup_cancelled) when *cancel became true, Error(tool_missing) when cmake (or curl) is not found, and
// Error(io_write) for file-system failures. A failed checksum deletes the partial file; everything else keeps
// it for the next call to resume.
DownloadResult download_toolchain(const DownloadOptions& options);

// SHA-256, exposed for tests and for callers that verify other pinned files.
class Sha256 {
  public:
    Sha256() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    std::array<std::uint8_t, 32> finish() noexcept;

  private:
    void block(const std::uint8_t* p) noexcept;
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

std::string sha256_hex(std::string_view data);
// The file:// URL of a local file (tests, and mirrors on a local disk).
std::string file_url(const std::filesystem::path& file);
// The file's digest as lowercase hex; `on_bytes` (optional) sees the running byte count. Throws
// Error(io_open) or Error(io_read).
std::string sha256_file(const std::filesystem::path& file,
                        const std::function<void(std::uint64_t)>& on_bytes = {});

} // namespace dq8::core::toolchain
