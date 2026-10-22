// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/memcard/memory_card.h"

#include "dq8/core/hash.h"
#include "runtime/io/memcard/mc_path.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace dq8::rt::mc {

namespace {

std::uint32_t file_clusters(std::size_t bytes) {
    return static_cast<std::uint32_t>((bytes + cluster_bytes - 1) / cluster_bytes);
}

// A directory holds "." and ".." plus its slots, two 512-byte entries per cluster.
std::uint32_t dir_clusters(std::size_t slots) {
    return static_cast<std::uint32_t>((slots + 2 + 1) / 2);
}

std::uint32_t node_clusters(const Node& node) {
    if (!node.is_dir()) {
        return file_clusters(node.data.size());
    }
    std::uint32_t total = dir_clusters(node.slots.size());
    for (const auto& slot : node.slots) {
        if (slot) {
            total += node_clusters(*slot);
        }
    }
    return total;
}

std::uint32_t live_entries(const Node& dir) {
    return static_cast<std::uint32_t>(
        2 + std::count_if(dir.slots.begin(), dir.slots.end(), [](const auto& s) { return s != nullptr; }));
}

void put_time(std::uint8_t* out, const RtcDateTime& t) {
    out[0] = 0;
    out[1] = t.second;
    out[2] = t.minute;
    out[3] = t.hour;
    out[4] = t.day;
    out[5] = t.month;
    out[6] = static_cast<std::uint8_t>(t.year & 0xFFu);
    out[7] = static_cast<std::uint8_t>(t.year >> 8);
}

RtcDateTime get_time(const std::uint8_t* in) {
    RtcDateTime t;
    t.second = in[1];
    t.minute = in[2];
    t.hour = in[3];
    t.day = in[4];
    t.month = in[5];
    t.year = static_cast<std::uint16_t>(in[6] | (in[7] << 8));
    return t;
}

bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= max_name && name != "." && name != ".." &&
           name.find_first_of("/\\") == std::string_view::npos && !has_wildcard(name);
}

// Digest helpers: fixed-width little-endian fields.
void hash_u32(core::Sha1& h, std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                               static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    h.update(b);
}
void hash_bytes(core::Sha1& h, std::span<const std::uint8_t> bytes) {
    hash_u32(h, static_cast<std::uint32_t>(bytes.size()));
    h.update(bytes);
}
void hash_time(core::Sha1& h, const RtcDateTime& t) {
    std::uint8_t b[8];
    put_time(b, t);
    h.update(b);
}
void hash_node(core::Sha1& h, const Node& node) {
    hash_bytes(h, std::span(reinterpret_cast<const std::uint8_t*>(node.name.data()), node.name.size()));
    hash_u32(h, node.attr);
    hash_time(h, node.created);
    hash_time(h, node.modified);
    if (node.is_dir()) {
        hash_u32(h, static_cast<std::uint32_t>(node.slots.size()));
        for (const auto& slot : node.slots) {
            hash_u32(h, slot ? 1u : 0u);
            if (slot) {
                hash_node(h, *slot);
            }
        }
    } else {
        hash_bytes(h, node.data);
    }
}

} // namespace

std::array<std::uint8_t, dir_entry_size> encode_dir_entry(const DirEntry& entry) {
    std::array<std::uint8_t, dir_entry_size> out{};
    put_time(out.data() + 0x00, entry.created);
    put_time(out.data() + 0x08, entry.modified);
    std::memcpy(out.data() + 0x10, &entry.size, 4);
    std::memcpy(out.data() + 0x14, &entry.attr, 2);
    std::memcpy(out.data() + 0x20, entry.name.data(), std::min(entry.name.size(), max_name));
    return out;
}

DirEntry decode_dir_entry(std::span<const std::uint8_t> bytes) {
    DirEntry e;
    e.created = get_time(bytes.data() + 0x00);
    e.modified = get_time(bytes.data() + 0x08);
    std::memcpy(&e.size, bytes.data() + 0x10, 4);
    std::memcpy(&e.attr, bytes.data() + 0x14, 2);
    for (std::size_t i = 0x20; i < dir_entry_size && bytes[i] != 0; ++i) {
        e.name.push_back(static_cast<char>(bytes[i]));
    }
    return e;
}

const Node* Node::child(std::string_view child_name) const {
    for (const auto& slot : slots) {
        if (slot && slot->name == child_name) {
            return slot.get();
        }
    }
    return nullptr;
}

Node* Node::child(std::string_view child_name) {
    return const_cast<Node*>(std::as_const(*this).child(child_name));
}

MemoryCard::MemoryCard(bool formatted, const RtcDateTime& now) {
    erase(formatted, now);
}

void MemoryCard::load(bool formatted, Node root) {
    formatted_ = formatted;
    root_ = std::move(root);
    root_.name.clear();
    root_.attr = dir_attr;
    inserted_ = true;
    current_.clear();
    handles_ = {};
}

void MemoryCard::erase(bool formatted, const RtcDateTime& now) {
    formatted_ = formatted;
    root_ = Node{};
    root_.attr = dir_attr;
    root_.created = now;
    root_.modified = now;
    current_.clear();
    handles_ = {};
}

std::uint32_t MemoryCard::used_clusters() const {
    return formatted_ ? node_clusters(root_) : 0;
}

std::uint32_t MemoryCard::free_clusters() const {
    if (!formatted_) {
        return 0;
    }
    const std::uint32_t used = used_clusters();
    return used >= allocatable_clusters ? 0 : allocatable_clusters - used;
}

bool MemoryCard::room_for(std::uint32_t clusters) const {
    return clusters <= free_clusters();
}

std::string MemoryCard::handle_path(std::int32_t fd) const {
    if (fd < 0 || static_cast<std::size_t>(fd) >= handles_.size() ||
        !handles_[static_cast<std::size_t>(fd)].open) {
        return {};
    }
    return join_path(handles_[static_cast<std::size_t>(fd)].path);
}

void MemoryCard::report(Change change) {
    if (listener_ != nullptr) {
        listener_->card_changed(change);
    }
}

Node* MemoryCard::find(const std::vector<std::string>& components) {
    return const_cast<Node*>(std::as_const(*this).find(components));
}

const Node* MemoryCard::find(const std::vector<std::string>& components) const {
    const Node* node = &root_;
    for (const std::string& part : components) {
        if (!node->is_dir()) {
            return nullptr;
        }
        node = node->child(part);
        if (node == nullptr) {
            return nullptr;
        }
    }
    return node;
}

std::int32_t MemoryCard::lookup(std::string_view path, std::vector<std::string>& components, Node*& parent,
                                Node*& node) {
    parent = nullptr;
    node = nullptr;
    if (!formatted_) {
        return result_no_format;
    }
    components = resolve_path(path, current_);
    if (components.empty()) {
        node = &root_;
        return result_ok;
    }
    parent = find(std::vector<std::string>(components.begin(), components.end() - 1));
    if (parent == nullptr || !parent->is_dir()) {
        parent = nullptr;
        return result_no_entry;
    }
    node = parent->child(components.back());
    return result_ok;
}

std::int32_t MemoryCard::create(Node& parent, std::vector<std::string> path, bool directory,
                                const RtcDateTime& now) {
    const auto free_slot = std::find(parent.slots.begin(), parent.slots.end(), nullptr);
    std::uint32_t needed = directory ? dir_clusters(0) : 0;
    if (free_slot == parent.slots.end() &&
        dir_clusters(parent.slots.size() + 1) > dir_clusters(parent.slots.size())) {
        ++needed;
    }
    if (!room_for(needed)) {
        return result_full;
    }
    auto node = std::make_unique<Node>();
    node->name = path.back();
    node->attr = directory ? dir_attr : static_cast<std::uint16_t>(file_attr & ~attr_closed);
    node->created = now;
    node->modified = now;
    if (free_slot != parent.slots.end()) {
        *free_slot = std::move(node);
    } else {
        parent.slots.push_back(std::move(node));
    }
    parent.modified = now;
    report(Change{Change::Kind::created, std::move(path), {}, nullptr});
    return result_ok;
}

MemoryCard::Info MemoryCard::get_info() {
    Info info;
    info.result = inserted_ ? (formatted_ ? result_new_card : result_no_format) : result_ok;
    info.free_clusters = static_cast<std::int32_t>(free_clusters());
    info.format = formatted_ ? 1 : 0;
    inserted_ = false;
    return info;
}

std::int32_t MemoryCard::open(std::string_view path, std::uint32_t flags, const RtcDateTime& now) {
    std::vector<std::string> components;
    Node* parent = nullptr;
    Node* node = nullptr;
    if (const std::int32_t r = lookup(path, components, parent, node); r != result_ok) {
        return r;
    }
    if (parent == nullptr) {
        return result_denied; // the root itself
    }
    if (!valid_name(components.back())) {
        return result_no_entry;
    }
    if ((flags & open_mkdir) != 0) {
        if (node != nullptr) {
            return result_no_entry; // mcman's answer for an existing entry; the game treats it as "exists"
        }
        return create(*parent, components, true, now);
    }
    if (node != nullptr && node->is_dir()) {
        return result_denied;
    }
    const auto handle =
        std::find_if(handles_.begin(), handles_.end(), [](const Handle& h) { return !h.open; });
    if (handle == handles_.end()) {
        return result_handles;
    }
    if (node == nullptr) {
        if ((flags & open_create) == 0) {
            return result_no_entry;
        }
        if (const std::int32_t r = create(*parent, components, false, now); r != result_ok) {
            return r;
        }
        node = parent->child(components.back());
    }
    *handle = Handle{true, components, flags, 0, false};
    if ((flags & open_truncate) != 0 && (flags & open_write) != 0 && !node->data.empty()) {
        node->data.clear();
        handle->dirty = true;
    }
    return static_cast<std::int32_t>(handle - handles_.begin());
}

void MemoryCard::commit(std::int32_t fd, const RtcDateTime& now) {
    Handle& h = handles_[static_cast<std::size_t>(fd)];
    if (!h.dirty) {
        return;
    }
    h.dirty = false;
    if (Node* node = find(h.path); node != nullptr) {
        node->modified = now;
        report(Change{Change::Kind::file_written, h.path, {}, nullptr});
    }
}

std::int32_t MemoryCard::close(std::int32_t fd, const RtcDateTime& now) {
    if (handle_path(fd).empty()) {
        return result_no_entry;
    }
    commit(fd, now);
    Handle& h = handles_[static_cast<std::size_t>(fd)];
    if (Node* node = find(h.path); node != nullptr) {
        node->attr |= attr_closed;
    }
    h = Handle{};
    return result_ok;
}

std::int32_t MemoryCard::seek(std::int32_t fd, std::int32_t offset, std::uint32_t origin) {
    if (handle_path(fd).empty()) {
        return result_no_entry;
    }
    Handle& h = handles_[static_cast<std::size_t>(fd)];
    const Node* node = find(h.path);
    std::int64_t base = 0;
    if (origin == 1) {
        base = h.position;
    } else if (origin == 2) {
        base = node != nullptr ? static_cast<std::int64_t>(node->data.size()) : 0;
    }
    const std::int64_t position = base + offset;
    if (position < 0 || position > 0x7FFFFFFF) {
        return result_denied;
    }
    h.position = static_cast<std::uint32_t>(position);
    return static_cast<std::int32_t>(position);
}

std::int32_t MemoryCard::read(std::int32_t fd, std::uint32_t size, std::vector<std::uint8_t>& out) {
    out.clear();
    if (handle_path(fd).empty()) {
        return result_no_entry;
    }
    Handle& h = handles_[static_cast<std::size_t>(fd)];
    if ((h.flags & open_read) == 0) {
        return result_denied;
    }
    const Node* node = find(h.path);
    if (node == nullptr) {
        return result_no_entry;
    }
    const std::size_t available = h.position < node->data.size() ? node->data.size() - h.position : 0;
    const std::size_t n = std::min<std::size_t>(size, available);
    const auto first = node->data.begin() + static_cast<std::ptrdiff_t>(h.position);
    out.assign(first, first + static_cast<std::ptrdiff_t>(n));
    h.position += static_cast<std::uint32_t>(n);
    return static_cast<std::int32_t>(n);
}

std::int32_t MemoryCard::write(std::int32_t fd, std::span<const std::uint8_t> bytes) {
    if (handle_path(fd).empty()) {
        return result_no_entry;
    }
    Handle& h = handles_[static_cast<std::size_t>(fd)];
    if ((h.flags & open_write) == 0) {
        return result_denied;
    }
    Node* node = find(h.path);
    if (node == nullptr) {
        return result_no_entry;
    }
    std::size_t n = bytes.size();
    const std::size_t size = node->data.size();
    const std::size_t end = std::size_t{h.position} + n;
    if (end > size) {
        // Grow only as far as the free clusters allow; a write that cannot place a byte is "full".
        const std::size_t limit = (std::size_t{file_clusters(size)} + free_clusters()) * cluster_bytes;
        if (end > limit) {
            n = limit > h.position ? limit - h.position : 0;
            if (n == 0) {
                return result_full;
            }
        }
        node->data.resize(std::max<std::size_t>(size, std::size_t{h.position} + n), 0);
    }
    std::copy_n(bytes.begin(), n, node->data.begin() + static_cast<std::ptrdiff_t>(h.position));
    h.position += static_cast<std::uint32_t>(n);
    h.dirty = true;
    return static_cast<std::int32_t>(n);
}

std::int32_t MemoryCard::flush(std::int32_t fd, const RtcDateTime& now) {
    if (handle_path(fd).empty()) {
        return result_no_entry;
    }
    commit(fd, now);
    return result_ok;
}

std::int32_t MemoryCard::chdir(std::string_view path) {
    if (!formatted_) {
        return result_no_format;
    }
    std::vector<std::string> components = resolve_path(path, current_);
    const Node* node = find(components);
    if (node == nullptr || !node->is_dir()) {
        return result_no_entry;
    }
    current_ = std::move(components);
    return result_ok;
}

std::int32_t MemoryCard::get_dir(std::string_view path, std::uint32_t max, std::vector<DirEntry>& out) const {
    out.clear();
    if (!formatted_) {
        return result_no_format;
    }
    const std::vector<std::string> components = resolve_path(path, current_);
    if (components.empty()) {
        return 0;
    }
    const Node* dir = find(std::vector<std::string>(components.begin(), components.end() - 1));
    if (dir == nullptr || !dir->is_dir()) {
        return result_no_entry;
    }
    const std::string& pattern = components.back();
    const auto add = [&](DirEntry e) {
        if (out.size() < max && wildcard_match(pattern, e.name)) {
            out.push_back(std::move(e));
        }
    };
    add(DirEntry{dir->created, dir->modified, live_entries(*dir), dir_attr, "."});
    add(DirEntry{dir->created, dir->modified, 0, dir_attr, ".."});
    for (const auto& slot : dir->slots) {
        if (slot) {
            add(DirEntry{slot->created, slot->modified,
                         slot->is_dir() ? live_entries(*slot) : static_cast<std::uint32_t>(slot->data.size()),
                         slot->attr, slot->name});
        }
    }
    return static_cast<std::int32_t>(out.size());
}

std::int32_t MemoryCard::set_info(std::string_view path, const DirEntry& info, std::uint32_t flags) {
    std::vector<std::string> components;
    Node* parent = nullptr;
    Node* node = nullptr;
    if (const std::int32_t r = lookup(path, components, parent, node); r != result_ok) {
        return r;
    }
    if (node == nullptr) {
        return result_no_entry;
    }
    if (parent == nullptr) {
        return result_denied; // the root
    }
    if ((flags & info_rename) != 0) {
        if (!valid_name(info.name)) {
            return result_no_entry;
        }
        if (const Node* other = parent->child(info.name); other != nullptr && other != node) {
            return result_denied;
        }
    }
    bool info_changed = false;
    if ((flags & info_created) != 0) {
        node->created = info.created;
        info_changed = true;
    }
    if ((flags & info_modified) != 0) {
        node->modified = info.modified;
        info_changed = true;
    }
    if ((flags & info_attr) != 0) {
        node->attr = static_cast<std::uint16_t>((node->attr & ~settable_attr) | (info.attr & settable_attr));
        info_changed = true;
    }
    if (info_changed) {
        report(Change{Change::Kind::info, components, {}, nullptr});
    }
    if ((flags & info_rename) != 0 && info.name != node->name) {
        const std::vector<std::string> old_path = components;
        node->name = info.name;
        for (Handle& h : handles_) {
            if (h.open && h.path == old_path) {
                h.path.back() = info.name;
            }
        }
        report(Change{Change::Kind::renamed, old_path, info.name, nullptr});
    }
    return result_ok;
}

std::int32_t MemoryCard::remove(std::string_view path, const RtcDateTime& now) {
    std::vector<std::string> components;
    Node* parent = nullptr;
    Node* node = nullptr;
    if (const std::int32_t r = lookup(path, components, parent, node); r != result_ok) {
        return r;
    }
    if (node == nullptr) {
        return result_no_entry;
    }
    if (parent == nullptr) {
        return result_denied;
    }
    for (const Handle& h : handles_) {
        if (h.open && h.path.size() >= components.size() &&
            std::equal(components.begin(), components.end(), h.path.begin())) {
            return result_denied;
        }
    }
    if (node->is_dir() && live_entries(*node) > 2) {
        return result_not_empty;
    }
    for (auto& slot : parent->slots) {
        if (slot.get() == node) {
            std::unique_ptr<Node> gone = std::move(slot); // the slot stays, free for the next entry
            parent->modified = now;
            report(Change{Change::Kind::removed, components, {}, gone.get()});
            break;
        }
    }
    return result_ok;
}

std::int32_t MemoryCard::format(const RtcDateTime& now) {
    erase(true, now);
    report(Change{Change::Kind::formatted, {}, {}, nullptr});
    return result_ok;
}

std::int32_t MemoryCard::unformat() {
    erase(false, root_.modified);
    report(Change{Change::Kind::unformatted, {}, {}, nullptr});
    return result_ok;
}

std::string MemoryCard::sha1() const {
    core::Sha1 h;
    static constexpr char tag[] = "dq8-memory-card-v1";
    h.update(std::span(reinterpret_cast<const std::uint8_t*>(tag), sizeof tag - 1));
    hash_u32(h, formatted_ ? 1u : 0u);
    if (formatted_) {
        hash_node(h, root_);
    }
    const auto digest = h.finish();
    return core::to_hex(digest);
}

} // namespace dq8::rt::mc
