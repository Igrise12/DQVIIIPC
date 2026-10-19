// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace dq8::core {

// Streaming SHA-1 (RFC 3174). Used for file identity in region profiles and manifests, not for security.
class Sha1 {
  public:
    using Digest = std::array<std::uint8_t, 20>;

    Sha1() noexcept;
    void update(std::span<const std::uint8_t> data) noexcept;
    // Finalises and returns the digest. The object must not be updated afterwards; call reset() first.
    Digest finish() noexcept;
    void reset() noexcept;

    static Digest of(std::span<const std::uint8_t> data) noexcept;

  private:
    void process_block(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 5> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

// Streaming CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), the "zlib"/"7z" variant.
class Crc32 {
  public:
    Crc32() noexcept = default;
    void update(std::span<const std::uint8_t> data) noexcept;
    std::uint32_t value() const noexcept { return ~crc_; }
    void reset() noexcept { crc_ = 0xFFFFFFFFu; }

    static std::uint32_t of(std::span<const std::uint8_t> data) noexcept;

  private:
    std::uint32_t crc_ = 0xFFFFFFFFu;
};

// Lowercase hexadecimal without prefix: "f24bfd21..." for a digest, "a66a08b0" for a CRC.
std::string to_hex(std::span<const std::uint8_t> bytes);
std::string to_hex(std::uint32_t value);

// Lowercases an ASCII hex string so recorded and computed values compare equal. Non-hex input is returned
// lowercased as well; callers that need validation check the length and characters themselves.
std::string normalize_hex(std::string_view hex);

} // namespace dq8::core
