// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dq8::core::iso {
class Image;
}

// The disc as the IOP services see it (architecture §8.3): random access by sector and lookup by path. M3
// reads synchronously on the machine thread; completion times are virtual (CostModel::cdvd_read), so the I/O
// thread can come later without changing guest-visible behaviour. Implemented by M3 session C.
namespace dq8::rt {

struct DiscFile {
    std::string path;      // as found, ISO 9660 form ("BIN/TITLE.BIN")
    std::uint32_t lsn = 0; // first sector
    std::uint64_t size = 0;
};

class IDiscSource {
  public:
    static constexpr std::uint32_t sector_size = 2048;
    virtual ~IDiscSource() = default;
    virtual std::uint32_t sector_count() const = 0;
    // Case-insensitive; '/' or '\' separators, optional leading separator and ";1" version. Device prefixes
    // ("cdrom0:") are the caller's to strip.
    virtual std::optional<DiscFile> find(std::string_view path) const = 0;
    // `count` whole sectors from `lsn`; out.size() == count * sector_size. Throws core::Error past the end.
    virtual void read_sectors(std::uint32_t lsn, std::uint32_t count, std::span<std::uint8_t> out) const = 0;
};

// An IDiscSource over a verified ISO image. The image must outlive the source.
std::unique_ptr<IDiscSource> make_iso_disc_source(const core::iso::Image& image);

} // namespace dq8::rt
