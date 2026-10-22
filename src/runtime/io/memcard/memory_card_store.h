// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/io/memcard/memory_card.h"
#include "runtime/machine/rtc.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// One memory card behind the native mcserv (ADR-0016; IopConfig::cards). The card model (memory_card.h) is
// authoritative for the guest; the store keeps it and, for a folder card, writes its changes behind the
// guest's back on an I/O thread with atomic replaces and rolling backups
// (Docs/formats/memory-card-folder.md). Disk speed and disk errors never reach the guest: a failed write is
// reported on the host side only.
namespace dq8::rt {

class IMemoryCardStore {
  public:
    virtual ~IMemoryCardStore() = default;
    // The card the guest sees. Machine thread only.
    virtual mc::MemoryCard& card() = 0;
    // Waits until every change made so far is on disk (nothing to wait for in memory). Call it when the
    // machine thread is not running.
    virtual void finish() = 0;
    // "memory", or the card's folder.
    virtual std::string describe() const = 0;
    // Writes that failed so far (each was also reported through the warning sink).
    virtual std::uint64_t write_errors() const = 0;
};

// A card that lives only in memory (headless runs by default): formatted with an empty root, or unformatted.
std::unique_ptr<IMemoryCardStore> make_memory_card_store(bool formatted, const RtcDateTime& now);

struct FolderStoreOptions {
    // Generations kept in .backup/ for every replaced or deleted file.
    std::size_t backups = 3;
    // Host-side warnings (loading problems, failed writes); default: stderr. Called on the I/O thread too.
    std::function<void(const std::string&)> warn;
    // Tests only: called on the I/O thread at the named points of a write ("temp-written" after a file's
    // new bytes are in .tmp/, "meta-temp-written" for a directory's metadata); throwing abandons the write
    // there, as if the process had died.
    std::function<void(std::string_view point)> fault;
};

// The card in `folder` (<save-dir>/<profile>/card1/). A folder without card.toml is a new card, formatted
// or not as `seed_formatted` says, stamped `now`; nothing is written until the guest changes it. Throws
// std::runtime_error when the folder cannot be used (not a directory, card.toml unreadable or of an unknown
// version).
std::unique_ptr<IMemoryCardStore> open_folder_card_store(const std::filesystem::path& folder,
                                                         bool seed_formatted, const RtcDateTime& now,
                                                         FolderStoreOptions options = {});

// The host file name a card entry is stored under: the name itself when it is safe on every host file
// system, else with the unsafe bytes as %XX (a leading or trailing '.' or space, '%', bytes outside printable
// ASCII, characters Windows forbids, Windows device names).
std::string host_file_name(std::string_view card_name);
// The reverse, for files found in a folder without metadata; empty when `host` is not a valid encoding.
std::string card_name_from_host(std::string_view host);

} // namespace dq8::rt
