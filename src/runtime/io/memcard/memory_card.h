// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include "runtime/machine/rtc.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The memory card as the guest sees it (ADR-0016): an 8 MB PS2 card held in memory, with mcman's file
// system semantics (directories with "." and ".." first, entries in card order with freed slots reused,
// cluster accounting for the free space, three file handles, the results the game's libmc distinguishes).
// It alone decides what the guest observes; persistence (memory_card_store.h) follows its changes and never
// feeds back. Machine thread only. The layouts and results are in Docs/milestones/M4-playable.md, "Protocol
// notes".
namespace dq8::rt::mc {

// Results.
inline constexpr std::int32_t result_ok = 0;
inline constexpr std::int32_t result_new_card =
    -1; // GetInfo: a card inserted since the last GetInfo, formatted
inline constexpr std::int32_t result_no_format =
    -2; // GetInfo: newly inserted and unformatted; others: unformatted
inline constexpr std::int32_t result_full = -3;
inline constexpr std::int32_t result_no_entry = -4;
inline constexpr std::int32_t result_denied = -5;
inline constexpr std::int32_t result_not_empty = -6;
inline constexpr std::int32_t result_handles = -7;
inline constexpr std::int32_t result_no_card = -12;

// Open flags.
inline constexpr std::uint32_t open_read = 0x0001;
inline constexpr std::uint32_t open_write = 0x0002;
inline constexpr std::uint32_t open_access = 0x0003;
inline constexpr std::uint32_t open_mkdir = 0x0040;
inline constexpr std::uint32_t open_create = 0x0200;
inline constexpr std::uint32_t open_truncate = 0x0400;

// Entry attributes.
inline constexpr std::uint16_t attr_read = 0x0001;
inline constexpr std::uint16_t attr_write = 0x0002;
inline constexpr std::uint16_t attr_execute = 0x0004;
inline constexpr std::uint16_t attr_protect = 0x0008;
inline constexpr std::uint16_t attr_file = 0x0010;
inline constexpr std::uint16_t attr_dir = 0x0020;
inline constexpr std::uint16_t attr_closed = 0x0080;
inline constexpr std::uint16_t attr_ps2 = 0x0400;
inline constexpr std::uint16_t attr_hidden = 0x2000;
inline constexpr std::uint16_t attr_exists = 0x8000;
inline constexpr std::uint16_t dir_attr =
    attr_exists | attr_ps2 | attr_dir | attr_execute | attr_write | attr_read;
inline constexpr std::uint16_t file_attr =
    attr_exists | attr_ps2 | attr_closed | attr_file | attr_execute | attr_write | attr_read;
// The bits SetInfo's attribute flag may change.
inline constexpr std::uint16_t settable_attr =
    attr_hidden | attr_protect | attr_execute | attr_write | attr_read;

// SetInfo flags.
inline constexpr std::uint32_t info_created = 0x01;
inline constexpr std::uint32_t info_modified = 0x02;
inline constexpr std::uint32_t info_attr = 0x04;
inline constexpr std::uint32_t info_rename = 0x10;

// Capacity: 1 KiB clusters; what a freshly formatted card offers (its root directory takes one of them).
inline constexpr std::uint32_t cluster_bytes = 1024;
inline constexpr std::uint32_t allocatable_clusters = 8001;
inline constexpr std::size_t max_name = 31; // bytes, without the terminator
inline constexpr std::size_t max_handles = 3;
inline constexpr std::int32_t card_type_ps2 = 2;

// One GetDir record (0x40 bytes on the wire: dir_entry_size).
struct DirEntry {
    RtcDateTime created;
    RtcDateTime modified;
    std::uint32_t size = 0; // bytes for a file, entries for a directory
    std::uint16_t attr = 0;
    std::string name;
};
inline constexpr std::size_t dir_entry_size = 0x40;
std::array<std::uint8_t, dir_entry_size> encode_dir_entry(const DirEntry& entry);
DirEntry decode_dir_entry(std::span<const std::uint8_t> bytes); // at least dir_entry_size bytes

// A file or directory. Directories keep their entries in card order; a null slot is a deleted entry that
// the next new entry reuses. "." and ".." are implicit (always listed first).
struct Node {
    std::string name;
    std::uint16_t attr = 0;
    RtcDateTime created;
    RtcDateTime modified;
    std::vector<std::uint8_t> data;           // a file's bytes
    std::vector<std::unique_ptr<Node>> slots; // a directory's entries
    // The folder store's host file name for this entry (empty until stored); never visible to the guest.
    std::string host;

    bool is_dir() const noexcept { return (attr & attr_dir) != 0; }
    const Node* child(std::string_view child_name) const;
    Node* child(std::string_view child_name);
};

// What changed, for persistence: reported at the points where a real card has the change on flash.
struct Change {
    enum class Kind {
        file_written, // a file's bytes (close or flush after writes)
        created,      // a new file or directory (`path`)
        removed,      // `path` was deleted (`node` is the removed entry, still valid during the call)
        renamed,      // `path` (the old path) is now `new_name` in the same directory
        info,         // times or attributes of `path` changed
        formatted,    // everything was erased and the card is formatted
        unformatted,  // everything was erased and the card is unformatted
    };
    Kind kind = Kind::file_written;
    std::vector<std::string> path; // components
    std::string new_name;
    const Node* node = nullptr;
};

class IChangeListener {
  public:
    virtual ~IChangeListener() = default;
    virtual void card_changed(const Change& change) = 0;
};

class MemoryCard {
  public:
    struct Info {
        std::int32_t result = result_ok;
        std::int32_t type = card_type_ps2;
        std::int32_t free_clusters = 0;
        std::int32_t format = 0; // 1 formatted, 0 not
    };

    // A card just inserted: formatted with an empty root stamped `now`, or unformatted.
    MemoryCard(bool formatted, const RtcDateTime& now);
    MemoryCard(const MemoryCard&) = delete;
    MemoryCard& operator=(const MemoryCard&) = delete;

    void set_listener(IChangeListener* listener) noexcept { listener_ = listener; }

    bool formatted() const noexcept { return formatted_; }
    const Node& root() const noexcept { return root_; }
    Node& root() noexcept { return root_; }
    std::uint32_t used_clusters() const;
    std::uint32_t free_clusters() const;
    // The path of an open handle, for reports; empty when `fd` is not open.
    std::string handle_path(std::int32_t fd) const;

    // ---- The operations mcserv translates. Each returns a result (or a handle, count or position >= 0).
    // The first GetInfo after insertion answers new_card or no_format, every later one result_ok.
    Info get_info();
    std::int32_t open(std::string_view path, std::uint32_t flags, const RtcDateTime& now);
    std::int32_t close(std::int32_t fd, const RtcDateTime& now);
    std::int32_t seek(std::int32_t fd, std::int32_t offset, std::uint32_t origin);
    std::int32_t read(std::int32_t fd, std::uint32_t size, std::vector<std::uint8_t>& out);
    std::int32_t write(std::int32_t fd, std::span<const std::uint8_t> bytes);
    std::int32_t flush(std::int32_t fd, const RtcDateTime& now);
    std::int32_t chdir(std::string_view path);
    // Mode 0 listing: the entries of the parent of `path` whose names match its last component (wildcards),
    // at most `max` of them.
    std::int32_t get_dir(std::string_view path, std::uint32_t max, std::vector<DirEntry>& out) const;
    std::int32_t set_info(std::string_view path, const DirEntry& info, std::uint32_t flags);
    std::int32_t remove(std::string_view path, const RtcDateTime& now);
    std::int32_t format(const RtcDateTime& now);
    std::int32_t unformat();

    // A canonical digest of what the guest can observe (format state and the tree: names, attributes, times,
    // bytes, slot order), lowercase hex: the run's card_sha1.
    std::string sha1() const;

    // Loading (the folder store): replaces the content without any change report and marks the card as just
    // inserted.
    void load(bool formatted, Node root);

  private:
    struct Handle {
        bool open = false;
        std::vector<std::string> path;
        std::uint32_t flags = 0;
        std::uint32_t position = 0;
        bool dirty = false;
    };

    std::int32_t lookup(std::string_view path, std::vector<std::string>& components, Node*& parent,
                        Node*& node);
    Node* find(const std::vector<std::string>& components);
    const Node* find(const std::vector<std::string>& components) const;
    std::int32_t create(Node& parent, std::vector<std::string> path, bool directory, const RtcDateTime& now);
    bool room_for(std::uint32_t clusters) const;
    void commit(std::int32_t fd, const RtcDateTime& now);
    void report(Change change);
    void erase(bool formatted, const RtcDateTime& now);

    bool formatted_ = false;
    bool inserted_ = true; // GetInfo has not reported the insertion yet
    Node root_;
    std::vector<std::string> current_; // the current directory (chdir)
    std::array<Handle, max_handles> handles_{};
    IChangeListener* listener_ = nullptr;
};

} // namespace dq8::rt::mc
