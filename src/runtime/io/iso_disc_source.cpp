// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/error.h"
#include "dq8/core/iso9660.h"
#include "runtime/io/disc_source.h"

#include <string>

namespace dq8::rt {

namespace {

class IsoDiscSource final : public IDiscSource {
  public:
    explicit IsoDiscSource(const core::iso::Image& image) : image_(image) {}

    std::uint32_t sector_count() const override { return image_.volume_sectors(); }

    std::optional<DiscFile> find(std::string_view path) const override {
        const core::iso::Entry* entry = image_.find(path);
        if (entry == nullptr || entry->is_directory()) {
            return std::nullopt;
        }
        std::string p = entry->path;
        if (!p.empty() && p.front() == '/') {
            p.erase(0, 1);
        }
        return DiscFile{std::move(p), entry->lba, entry->size};
    }

    void read_sectors(std::uint32_t lsn, std::uint32_t count, std::span<std::uint8_t> out) const override {
        if (out.size() != static_cast<std::size_t>(count) * sector_size) {
            throw core::Error(core::ErrorCode::iso_range, "disc read of " + std::to_string(count) +
                                                              " sectors into a buffer of " +
                                                              std::to_string(out.size()) + " bytes");
        }
        if (count == 0) {
            return;
        }
        if (std::uint64_t{lsn} + count > image_.volume_sectors()) {
            throw core::Error(core::ErrorCode::iso_range, "disc read of sectors " + std::to_string(lsn) +
                                                              " to " +
                                                              std::to_string(std::uint64_t{lsn} + count - 1) +
                                                              " past the end of the volume");
        }
        image_.read_sectors(lsn, count, out);
    }

  private:
    const core::iso::Image& image_;
};

} // namespace

std::unique_ptr<IDiscSource> make_iso_disc_source(const core::iso::Image& image) {
    return std::make_unique<IsoDiscSource>(image);
}

} // namespace dq8::rt
