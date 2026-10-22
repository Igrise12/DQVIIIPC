// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/io/memcard/memory_card_store.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <toml.hpp>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

// The folder format is Docs/formats/memory-card-folder.md: card.toml, one .dir.toml per directory with the
// entries in card order, the entries' bytes as plain files, .backup/ and .tmp/.
namespace dq8::rt {

namespace fs = std::filesystem;

namespace {

constexpr int folder_version = 1;
constexpr const char* card_file = "card.toml";
constexpr const char* dir_file = ".dir.toml";
constexpr const char* backup_dir = ".backup";
constexpr const char* temp_dir = ".tmp";
constexpr std::uintmax_t max_file_bytes = 8u << 20; // nothing on an 8 MB card is larger

// ---- Host names
// --------------------------------------------------------------------------------------------

bool windows_device_name(std::string_view name) {
    std::string base(name.substr(0, name.find('.')));
    for (char& c : base) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return true;
    }
    return base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && base[3] >= '1' &&
           base[3] <= '9';
}

bool safe_byte(unsigned char c) {
    if (c < 0x20 || c >= 0x7F) {
        return false;
    }
    return std::string_view("<>:\"/\\|?*%").find(static_cast<char>(c)) == std::string_view::npos;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// ---- TOML text
// ---------------------------------------------------------------------------------------------

// A TOML basic string. Bytes outside printable ASCII become \u00XX, which reading maps back to the byte.
std::string toml_string(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c >= 0x7F) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04X", static_cast<unsigned>(c));
            out += esc;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

// The bytes of a string written by toml_string(): its UTF-8 decoded, every code point below 0x100 one byte.
std::optional<std::string> bytes_of(const std::string& utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size() && c <= 0xC3) {
            out.push_back(
                static_cast<char>(((c & 0x1F) << 6) | (static_cast<unsigned char>(utf8[++i]) & 0x3F)));
        } else {
            return std::nullopt;
        }
    }
    return out;
}

std::string hex16(std::uint16_t v) {
    char text[8];
    std::snprintf(text, sizeof text, "0x%04X", static_cast<unsigned>(v));
    return text;
}

std::string dir_meta_text(const mc::Node& dir) {
    std::ostringstream out;
    out << "# DQ8-Native memory card directory (Docs/formats/memory-card-folder.md)\n"
        << "version = " << folder_version << "\n"
        << "attr = " << hex16(dir.attr) << "\n"
        << "created = " << toml_string(format_rtc(dir.created)) << "\n"
        << "modified = " << toml_string(format_rtc(dir.modified)) << "\n";
    for (const auto& slot : dir.slots) {
        out << "\n[[entry]]\n";
        if (!slot) {
            out << "kind = \"free\"\n";
            continue;
        }
        out << "name = " << toml_string(slot->name) << "\n";
        if (slot->host != slot->name) {
            out << "file = " << toml_string(slot->host) << "\n";
        }
        out << "kind = " << (slot->is_dir() ? "\"dir\"" : "\"file\"") << "\n"
            << "attr = " << hex16(slot->attr) << "\n"
            << "created = " << toml_string(format_rtc(slot->created)) << "\n"
            << "modified = " << toml_string(format_rtc(slot->modified)) << "\n";
    }
    return out.str();
}

std::string card_meta_text(bool formatted) {
    std::ostringstream out;
    out << "# DQ8-Native memory card (Docs/formats/memory-card-folder.md)\n"
        << "format = \"dq8-memory-card\"\n"
        << "version = " << folder_version << "\n"
        << "formatted = " << (formatted ? "true" : "false") << "\n";
    return out.str();
}

// ---- Durable file writes
// -----------------------------------------------------------------------------------

void write_durably(const fs::path& file, std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    HANDLE h =
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot create " + file.string());
    }
    std::size_t done = 0;
    bool ok = true;
    while (ok && done < bytes.size()) {
        DWORD wrote = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20));
        ok = WriteFile(h, bytes.data() + done, chunk, &wrote, nullptr) != 0;
        done += wrote;
    }
    ok = ok && FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (!ok) {
        throw std::runtime_error("cannot write " + file.string());
    }
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error("cannot create " + file.string() + ": " + std::strerror(errno));
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            ::close(fd);
            throw std::runtime_error("cannot write " + file.string() + ": " + std::strerror(error));
        }
        done += static_cast<std::size_t>(n);
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (!synced) {
        throw std::runtime_error("cannot sync " + file.string());
    }
#endif
}

// Moves `from` over `to` in one step: the target holds either its old or its new content, never neither.
void replace_file(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        throw std::runtime_error("cannot replace " + to.string());
    }
#else
    if (::rename(from.c_str(), to.c_str()) != 0) {
        throw std::runtime_error("cannot replace " + to.string() + ": " + std::strerror(errno));
    }
    if (const int dir = ::open(to.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dir >= 0) {
        ::fsync(dir); // the rename itself on disk; best effort
        ::close(dir);
    }
#endif
}

std::vector<std::uint8_t> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + file.string());
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes;
}

std::span<const std::uint8_t> text_bytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// ---- The in-memory store
// ------------------------------------------------------------------------------------

class MemoryStore final : public IMemoryCardStore {
  public:
    MemoryStore(bool formatted, const RtcDateTime& now) : card_(formatted, now) {}
    mc::MemoryCard& card() override { return card_; }
    void finish() override {}
    std::string describe() const override { return "memory"; }
    std::uint64_t write_errors() const override { return 0; }

  private:
    mc::MemoryCard card_;
};

// ---- The folder store
// ---------------------------------------------------------------------------------------

// Writes on the I/O thread, in the order the guest made the changes, so the folder always holds a state the
// card went through (each file and metadata file replaced atomically).
class FolderStore final : public IMemoryCardStore, public mc::IChangeListener {
  public:
    FolderStore(fs::path folder, bool seed_formatted, const RtcDateTime& now, FolderStoreOptions options)
        : folder_(std::move(folder)), options_(std::move(options)), card_(seed_formatted, now) {
        if (!options_.warn) {
            options_.warn = [](const std::string& message) {
                std::fprintf(stderr, "memory card: %s\n", message.c_str());
            };
        }
        load(now);
        card_.set_listener(this);
    }

    ~FolderStore() override {
        finish();
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    mc::MemoryCard& card() override { return card_; }
    std::string describe() const override { return folder_.string(); }
    std::uint64_t write_errors() const override { return errors_.load(); }

    void finish() override {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
    }

    // ---- Changes (machine thread): what to write is decided and copied here; the I/O thread only writes.
    void card_changed(const mc::Change& change) override {
        using Kind = mc::Change::Kind;
        if (change.kind == Kind::formatted || change.kind == Kind::unformatted) {
            const bool formatted = change.kind == Kind::formatted;
            enqueue([this] { wipe(); });
            write_card_meta(formatted);
            if (formatted) {
                put_meta({}, card_.root());
            }
            return;
        }
        if (!disk_formatted_ || !*disk_formatted_) {
            write_card_meta(true);
        }
        const std::vector<std::string> parent_path(change.path.begin(), change.path.end() - 1);
        mc::Node* parent = node_at(parent_path);
        if (parent == nullptr) {
            return;
        }
        const fs::path parent_rel = host_path(parent_path);
        switch (change.kind) {
        case Kind::file_written: {
            if (const mc::Node* node = parent->child(change.path.back()); node != nullptr) {
                put_file(parent_rel / node->host, node->data);
            }
            break;
        }
        case Kind::created: {
            mc::Node* node = parent->child(change.path.back());
            if (node == nullptr) {
                return;
            }
            node->host = unique_host(*parent, *node);
            const fs::path rel = parent_rel / node->host;
            if (node->is_dir()) {
                enqueue([this, rel] { fs::create_directories(folder_ / rel); });
                put_meta(rel, *node);
            } else {
                put_file(rel, node->data);
            }
            break;
        }
        case Kind::removed: {
            const fs::path rel = parent_rel / change.node->host;
            const bool dir = change.node->is_dir();
            enqueue([this, rel, dir] { remove_entry(rel, dir); });
            break;
        }
        case Kind::renamed: {
            mc::Node* node = parent->child(change.new_name);
            if (node == nullptr) {
                return;
            }
            const std::string old_host = node->host;
            node->host = unique_host(*parent, *node);
            if (node->host != old_host) {
                enqueue([this, from = parent_rel / old_host, to = parent_rel / node->host] {
                    fs::rename(folder_ / from, folder_ / to);
                });
            }
            break;
        }
        case Kind::info:
        case Kind::formatted:
        case Kind::unformatted:
            break;
        }
        put_meta(parent_rel, *parent); // the parent's entry list, times and attributes
    }

  private:
    // ---- Loading (constructor, before any thread).
    void load(const RtcDateTime& now) {
        if (fs::exists(folder_) && !fs::is_directory(folder_)) {
            throw std::runtime_error("memory card folder " + folder_.string() + " is not a directory");
        }
        const fs::path meta = folder_ / card_file;
        if (!fs::exists(meta)) {
            return; // a new card: nothing on disk until the guest changes it
        }
        toml::value card;
        try {
            card = toml::parse(meta);
        } catch (const std::exception& e) {
            throw std::runtime_error("memory card " + meta.string() + " cannot be read: " + e.what());
        }
        if (!card.is_table() || !card.contains("version") || !card.at("version").is_integer() ||
            card.at("version").as_integer() != folder_version) {
            throw std::runtime_error("memory card " + meta.string() + " is not a version " +
                                     std::to_string(folder_version) + " card");
        }
        const bool formatted = card.contains("formatted") && card.at("formatted").is_boolean() &&
                               card.at("formatted").as_boolean();
        disk_formatted_ = formatted;
        std::error_code ec;
        fs::remove_all(folder_ / temp_dir, ec); // writes a previous run did not finish
        mc::Node root;
        root.attr = mc::dir_attr;
        root.created = now;
        root.modified = now;
        if (formatted) {
            load_dir(folder_, root, now);
        }
        card_.load(formatted, std::move(root));
        if (card_.used_clusters() > mc::allocatable_clusters) {
            options_.warn(folder_.string() + " holds more than an 8 MB card can (" +
                          std::to_string(card_.used_clusters()) +
                          " clusters); the card reports no free space");
        }
    }

    void read_times(const toml::value& t, mc::Node& node) {
        const auto time = [&](const char* key, RtcDateTime& out) {
            if (t.contains(key) && t.at(key).is_string()) {
                if (const auto v = parse_rtc(t.at(key).as_string())) {
                    out = *v;
                }
            }
        };
        time("created", node.created);
        time("modified", node.modified);
        if (t.contains("attr") && t.at("attr").is_integer()) {
            node.attr = static_cast<std::uint16_t>(t.at("attr").as_integer());
        }
    }

    // Fills `dir` (its own times and attributes already set) from the host directory `path`.
    void load_dir(const fs::path& path, mc::Node& dir, const RtcDateTime& now) {
        std::set<std::string> used{dir_file};
        if (const fs::path meta = path / dir_file; fs::exists(meta)) {
            try {
                const toml::value t = toml::parse(meta);
                read_times(t, dir);
                dir.attr = static_cast<std::uint16_t>((dir.attr & ~mc::attr_file) | mc::attr_dir);
                if (t.contains("entry") && t.at("entry").is_array()) {
                    for (const toml::value& e : t.at("entry").as_array()) {
                        load_entry(path, dir, e, used, now);
                    }
                }
            } catch (const std::exception& e) {
                options_.warn(meta.string() + " cannot be read (" + e.what() +
                              "); its entries are taken from the folder in name order");
            }
        }
        // Files and folders the metadata does not list (copied in by hand, or written just before the
        // metadata): appended in name order.
        std::vector<fs::path> extra;
        for (const fs::directory_entry& e : fs::directory_iterator(path)) {
            const std::string host = e.path().filename().string();
            if (host.empty() || host[0] == '.' || used.count(host) != 0 ||
                (path == folder_ && host == card_file)) {
                continue;
            }
            extra.push_back(e.path());
        }
        std::sort(extra.begin(), extra.end());
        for (const fs::path& p : extra) {
            const std::string host = p.filename().string();
            const std::string name = card_name_from_host(host);
            if (name.empty() || name.size() > mc::max_name || dir.child(name) != nullptr) {
                options_.warn(p.string() + " is not a valid card entry name; ignored");
                continue;
            }
            auto node = std::make_unique<mc::Node>();
            node->name = name;
            node->host = host;
            node->created = dir.modified;
            node->modified = dir.modified;
            if (!fill_node(p, *node, now)) {
                continue;
            }
            dir.slots.push_back(std::move(node));
        }
    }

    void load_entry(const fs::path& path, mc::Node& dir, const toml::value& e, std::set<std::string>& used,
                    const RtcDateTime& now) {
        if (!e.is_table()) {
            return;
        }
        const std::string kind =
            e.contains("kind") && e.at("kind").is_string() ? e.at("kind").as_string() : "";
        if (kind == "free") {
            dir.slots.push_back(nullptr);
            return;
        }
        const auto name = e.contains("name") && e.at("name").is_string() ? bytes_of(e.at("name").as_string())
                                                                         : std::nullopt;
        if (!name || name->empty() || name->size() > mc::max_name || dir.child(*name) != nullptr) {
            options_.warn(path.string() + ": an entry with a missing, invalid or repeated name; ignored");
            return;
        }
        auto node = std::make_unique<mc::Node>();
        node->name = *name;
        node->host =
            e.contains("file") && e.at("file").is_string() ? e.at("file").as_string() : host_file_name(*name);
        node->attr = kind == "dir" ? mc::dir_attr : mc::file_attr;
        read_times(e, *node);
        const fs::path p = path / node->host;
        if (node->host.empty() || node->host[0] == '.' || !fs::exists(p) ||
            fs::is_directory(p) != (kind == "dir")) {
            options_.warn(p.string() + " is listed in its folder's metadata but missing; dropped");
            return;
        }
        used.insert(node->host);
        if (fill_node(p, *node, now)) {
            dir.slots.push_back(std::move(node));
        }
    }

    bool fill_node(const fs::path& p, mc::Node& node, const RtcDateTime& now) {
        if (fs::is_directory(p)) {
            node.attr = static_cast<std::uint16_t>((node.attr & ~mc::attr_file) | mc::attr_dir);
            load_dir(p, node, now);
            return true;
        }
        if (!fs::is_regular_file(p) || fs::file_size(p) > max_file_bytes) {
            options_.warn(p.string() + " is not a regular file of at most 8 MB; ignored");
            return false;
        }
        node.attr = static_cast<std::uint16_t>((node.attr & ~mc::attr_dir) | mc::attr_file | mc::attr_closed);
        node.data = read_bytes(p);
        return true;
    }

    // ---- Machine-thread helpers.
    mc::Node* node_at(const std::vector<std::string>& path) {
        mc::Node* node = &card_.root();
        for (const std::string& part : path) {
            node = node->child(part);
            if (node == nullptr) {
                return nullptr;
            }
        }
        return node;
    }

    fs::path host_path(const std::vector<std::string>& path) {
        fs::path rel;
        const mc::Node* node = &card_.root();
        for (const std::string& part : path) {
            node = node->child(part);
            if (node == nullptr) {
                break;
            }
            rel /= node->host;
        }
        return rel;
    }

    // A host name for `node` no sibling uses, compared without case (Windows and macOS folders).
    static std::string unique_host(const mc::Node& parent, const mc::Node& node) {
        const std::string base = host_file_name(node.name);
        std::set<std::string> taken;
        for (const auto& slot : parent.slots) {
            if (slot && slot.get() != &node) {
                taken.insert(lower(slot->host));
            }
        }
        std::string host = base;
        for (int n = 2; taken.count(lower(host)) != 0; ++n) {
            host = base + "~" + std::to_string(n);
        }
        return host;
    }

    void write_card_meta(bool formatted) {
        disk_formatted_ = formatted;
        enqueue([this, text = card_meta_text(formatted)] {
            fs::create_directories(folder_);
            put_text(card_file, text, "card-temp-written");
        });
    }

    void put_meta(const fs::path& rel_dir, const mc::Node& dir) {
        enqueue([this, rel = rel_dir / dir_file, text = dir_meta_text(dir)] {
            put_text(rel, text, "meta-temp-written");
        });
    }

    void put_file(const fs::path& rel, std::vector<std::uint8_t> bytes) {
        enqueue([this, rel, bytes = std::move(bytes)] { write_entry(rel, bytes); });
    }

    // ---- I/O thread.
    void enqueue(std::function<void()> job) {
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
            if (!worker_.joinable()) {
                worker_ = std::thread([this] { run(); });
            }
        }
        wake_.notify_one();
    }

    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return; // quit
            }
            std::function<void()> job = std::move(jobs_.front());
            jobs_.pop_front();
            busy_ = true;
            lock.unlock();
            try {
                job();
            } catch (const std::exception& e) {
                ++errors_;
                options_.warn(std::string("a write to ") + folder_.string() + " failed: " + e.what());
            }
            lock.lock();
            busy_ = false;
            if (jobs_.empty()) {
                idle_.notify_all();
            }
        }
    }

    fs::path temp_file() {
        fs::create_directories(folder_ / temp_dir);
        return folder_ / temp_dir / ("w" + std::to_string(++temp_serial_));
    }

    void put_text(const fs::path& rel, const std::string& text, std::string_view point) {
        const fs::path tmp = temp_file();
        write_durably(tmp, text_bytes(text));
        if (options_.fault) {
            options_.fault(point);
        }
        replace_file(tmp, folder_ / rel);
    }

    // A file's new bytes: the old content into .backup/ first (when it differs), then an atomic replace.
    void write_entry(const fs::path& rel, const std::vector<std::uint8_t>& bytes) {
        const fs::path target = folder_ / rel;
        if (fs::exists(target)) {
            if (fs::file_size(target) == bytes.size() && read_bytes(target) == bytes) {
                return;
            }
            backup(rel);
        }
        const fs::path tmp = temp_file();
        write_durably(tmp, bytes);
        if (options_.fault) {
            options_.fault("temp-written");
        }
        replace_file(tmp, target);
    }

    // .backup/<rel>.1 is the newest of `backups` generations.
    void backup(const fs::path& rel) {
        if (options_.backups == 0) {
            return;
        }
        const fs::path base = folder_ / backup_dir / rel;
        fs::create_directories(base.parent_path());
        const auto generation = [&](std::size_t n) {
            fs::path p = base;
            p += "." + std::to_string(n);
            return p;
        };
        std::error_code ec;
        fs::remove(generation(options_.backups), ec);
        for (std::size_t n = options_.backups; n > 1; --n) {
            if (fs::exists(generation(n - 1))) {
                fs::rename(generation(n - 1), generation(n));
            }
        }
        fs::copy_file(folder_ / rel, generation(1), fs::copy_options::overwrite_existing);
    }

    void backup_tree(const fs::path& rel) {
        const fs::path p = folder_ / rel;
        if (fs::is_directory(p)) {
            for (const fs::directory_entry& e : fs::directory_iterator(p)) {
                const std::string name = e.path().filename().string();
                if (name != dir_file) {
                    backup_tree(rel / name);
                }
            }
        } else if (fs::is_regular_file(p)) {
            backup(rel);
        }
    }

    void remove_entry(const fs::path& rel, bool dir) {
        const fs::path p = folder_ / rel;
        if (dir) {
            std::error_code ec;
            fs::remove(p / dir_file, ec);
            fs::remove(p); // empty on the card; anything left is not the card's and stays (an error)
        } else {
            if (fs::exists(p)) {
                backup(rel);
            }
            fs::remove(p);
        }
    }

    // Format and unformat: every entry goes, each file into .backup/ first.
    void wipe() {
        if (!fs::exists(folder_)) {
            return;
        }
        for (const fs::directory_entry& e : fs::directory_iterator(folder_)) {
            const std::string name = e.path().filename().string();
            if (name == backup_dir || name == temp_dir || name == card_file) {
                continue;
            }
            backup_tree(name);
            fs::remove_all(e.path());
        }
    }

    fs::path folder_;
    FolderStoreOptions options_;
    mc::MemoryCard card_;
    std::optional<bool> disk_formatted_; // card.toml's state as last written (machine thread)

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> jobs_;
    bool busy_ = false;
    bool quit_ = false;
    std::thread worker_;
    std::atomic<std::uint64_t> errors_{0};
    std::uint64_t temp_serial_ = 0; // I/O thread
};

} // namespace

std::string host_file_name(std::string_view card_name) {
    std::string out;
    for (std::size_t i = 0; i < card_name.size(); ++i) {
        const auto c = static_cast<unsigned char>(card_name[i]);
        const bool edge = i == 0 || i + 1 == card_name.size();
        const bool encode = !safe_byte(c) || (edge && (c == ' ' || c == '.')) || (i == 0 && c == '~') ||
                            (i == 0 && windows_device_name(card_name));
        if (encode) {
            char esc[4];
            std::snprintf(esc, sizeof esc, "%%%02X", static_cast<unsigned>(c));
            out += esc;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string card_name_from_host(std::string_view host) {
    std::string out;
    for (std::size_t i = 0; i < host.size(); ++i) {
        if (host[i] != '%') {
            out.push_back(host[i]);
            continue;
        }
        if (i + 2 >= host.size() || !std::isxdigit(static_cast<unsigned char>(host[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(host[i + 2]))) {
            return {};
        }
        out.push_back(static_cast<char>(std::stoi(std::string(host.substr(i + 1, 2)), nullptr, 16)));
        i += 2;
    }
    return out;
}

std::unique_ptr<IMemoryCardStore> make_memory_card_store(bool formatted, const RtcDateTime& now) {
    return std::make_unique<MemoryStore>(formatted, now);
}

std::unique_ptr<IMemoryCardStore> open_folder_card_store(const fs::path& folder, bool seed_formatted,
                                                         const RtcDateTime& now, FolderStoreOptions options) {
    return std::make_unique<FolderStore>(folder, seed_formatted, now, std::move(options));
}

} // namespace dq8::rt
