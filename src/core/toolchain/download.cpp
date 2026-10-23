// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "toolchain/download.h"

#include "dq8/core/error.h"
#include "dq8/core/process.h"
#include "platform/files.h"
#include "platform/paths.h"

#include <algorithm>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace dq8::core::toolchain {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

const DownloadPin& pinned_llvm_mingw() {
    // cmake/toolchains/llvm-mingw-pin.cmake: DQ8_LLVM_MINGW_VERSION, DQ8_LLVM_MINGW_WINDOWS_*.
    static const DownloadPin pin{
        "20261020",
        "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/"
        "llvm-mingw-20261020-ucrt-x86_64.zip",
        "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd",
        190612575,
        "llvm-mingw-20261020-ucrt-x86_64",
        749515686,
        9299,
    };
    return pin;
}

// ---------------------------------------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::array<std::uint32_t, 64> sha256_k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t b : digest) {
        out += digits[b >> 4];
        out += digits[b & 0xF];
    }
    return out;
}

} // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {
}

void Sha256::block(const std::uint8_t* p) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = static_cast<std::uint32_t>(p[4 * i]) << 24 | static_cast<std::uint32_t>(p[4 * i + 1]) << 16 |
               static_cast<std::uint32_t>(p[4 * i + 2]) << 8 | static_cast<std::uint32_t>(p[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t t1 =
            h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
        const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    length_ += size;
    while (size > 0) {
        if (buffered_ == 0 && size >= 64) {
            block(p);
            p += 64;
            size -= 64;
            continue;
        }
        const std::size_t n = std::min(size, buffer_.size() - buffered_);
        std::copy_n(p, n, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += n;
        p += n;
        size -= n;
        if (buffered_ == buffer_.size()) {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finish() noexcept {
    const std::uint64_t bits = length_ * 8;
    static constexpr std::uint8_t pad[64] = {0x80};
    const std::size_t pad_size = buffered_ < 56 ? 56 - buffered_ : 120 - buffered_;
    update(pad, pad_size);
    std::uint8_t length[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    update(length, sizeof length);
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            out[4 * i + j] = static_cast<std::uint8_t>(state_[i] >> (24 - 8 * j));
        }
    }
    return out;
}

std::string sha256_hex(std::string_view data) {
    Sha256 h;
    h.update(data.data(), data.size());
    return hex(h.finish());
}

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

std::string sha256_file(const fs::path& file, const std::function<void(std::uint64_t)>& on_bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::io_open, "cannot open '" + utf8(file) + "'");
    }
    Sha256 h;
    std::vector<char> buffer(std::size_t{1} << 20);
    std::uint64_t total = 0;
    for (;;) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            h.update(buffer.data(), static_cast<std::size_t>(got));
            total += static_cast<std::uint64_t>(got);
            if (on_bytes) {
                on_bytes(total);
            }
        }
        if (!in) {
            if (!in.eof()) {
                throw Error(ErrorCode::io_read, "cannot read '" + utf8(file) + "'");
            }
            break;
        }
    }
    return hex(h.finish());
}

// ---------------------------------------------------------------------------------------------------------------
// URLs and pins
// ---------------------------------------------------------------------------------------------------------------

namespace {

bool unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string percent_decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char a = s[i] >= 'A' && s[i] <= 'Z' ? static_cast<char>(s[i] - 'A' + 'a') : s[i];
        if (a != prefix[i]) {
            return false;
        }
    }
    return true;
}

// file:///abs/path, file://localhost/abs/path, file:///C:/dir/file -> a local path.
std::optional<fs::path> local_path(std::string_view url) {
    if (!starts_with_ci(url, "file://")) {
        return std::nullopt;
    }
    std::string_view rest = url.substr(7);
    if (starts_with_ci(rest, "localhost/")) {
        rest.remove_prefix(9);
    }
    std::string path = percent_decode(rest);
#if defined(_WIN32)
    // "/C:/dir" -> "C:/dir"
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') {
        path.erase(0, 1);
    }
#endif
    return fs::path(std::u8string(path.begin(), path.end()));
}

bool valid_name(std::string_view s) {
    return !s.empty() && s != "." && s != ".." && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '.' || c == '_';
    });
}

void validate(const DownloadPin& pin) {
    std::string problem;
    if (!valid_name(pin.version)) {
        problem = "the version '" + pin.version + "' is not a plain name";
    } else if (pin.sha256.size() != 64 || !std::all_of(pin.sha256.begin(), pin.sha256.end(), [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               })) {
        problem = "the SHA-256 is not 64 lowercase hex digits";
    } else if (!valid_name(pin.archive_root)) {
        problem = "the archive root '" + pin.archive_root + "' is not a plain directory name";
    } else if (!starts_with_ci(pin.url, "https://") && !starts_with_ci(pin.url, "http://") &&
               !starts_with_ci(pin.url, "file://")) {
        problem = "the URL is not http(s):// or file://";
    }
    if (!problem.empty()) {
        throw Error(ErrorCode::invalid_argument, "toolchain pin: " + problem);
    }
}

// The archive's file name: the URL's last path segment, or "<version>.zip".
std::string archive_name(const DownloadPin& pin) {
    std::string_view url = pin.url;
    url = url.substr(0, std::min(url.find('?'), url.find('#')));
    const std::size_t slash = url.rfind('/');
    const std::string name = percent_decode(slash == std::string_view::npos ? url : url.substr(slash + 1));
    return valid_name(name) ? name : pin.version + ".zip";
}

std::uint64_t size_or_zero(const fs::path& file) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

void remove_file(const fs::path& file) {
    std::error_code ec;
    fs::remove(file, ec);
    if (ec) {
        throw Error(ErrorCode::io_write, "cannot remove '" + utf8(file) + "': " + ec.message());
    }
}

void remove_tree(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) {
        throw Error(ErrorCode::io_write, "cannot remove '" + utf8(dir) + "': " + ec.message());
    }
}

// Renames a freshly unpacked directory. On Windows a virus scanner may still hold one of the new files for a
// moment, which makes MoveFileEx fail with access denied: retry for a few seconds.
void rename_into_place(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    for (int attempt = 0; attempt < 25; ++attempt) {
        fs::rename(from, to, ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(200ms);
    }
    throw Error(ErrorCode::io_write,
                "cannot move '" + utf8(from) + "' to '" + utf8(to) + "': " + ec.message());
}

constexpr std::uint64_t mib = std::uint64_t{1} << 20;

void check_space(const fs::path& dir, std::uint64_t need) {
    std::error_code ec;
    const fs::space_info info = fs::space(dir, ec);
    if (ec || info.available >= need) {
        return; // unknown: the writes report it
    }
    throw Error(ErrorCode::insufficient_space,
                "the toolchain needs " + std::to_string((need + mib - 1) / mib) + " MiB in '" + utf8(dir) +
                    "', but " + std::to_string(info.available / mib) + " MiB are free");
}

[[noreturn]] void throw_cancelled() {
    throw Error(ErrorCode::setup_cancelled,
                "the toolchain download was cancelled; the next attempt resumes it");
}

// ---------------------------------------------------------------------------------------------------------------
// Transports. Each appends to `part` from `offset`, or rewrites it from the start when the source ignores the
// range; download-failed for network and HTTP failures (the caller retries while bytes arrive).
// ---------------------------------------------------------------------------------------------------------------

struct Transfer {
    const DownloadOptions& options;
    std::uint64_t total;

    bool cancelled() const { return options.cancel != nullptr && options.cancel->load(); }
    void progress(std::uint64_t done) const {
        if (options.on_progress) {
            options.on_progress({DownloadPhase::download, done, total});
        }
    }
};

void fetch_local(const fs::path& source, const fs::path& part, std::uint64_t offset, const Transfer& t) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw Error(ErrorCode::download_failed, "cannot open '" + utf8(source) + "'");
    }
    if (offset > size_or_zero(source)) {
        offset = 0;
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::ofstream out(part, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
    if (!out) {
        throw Error(ErrorCode::io_write, "cannot write '" + utf8(part) + "'");
    }
    std::vector<char> buffer(std::size_t{64} << 10);
    std::uint64_t done = offset;
    for (;;) {
        if (t.cancelled()) {
            throw_cancelled();
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        out.write(buffer.data(), got);
        if (!out) {
            throw Error(ErrorCode::io_write, "cannot write '" + utf8(part) + "'");
        }
        out.flush();
        done += static_cast<std::uint64_t>(got);
        t.progress(done);
    }
    if (in.bad()) {
        throw Error(ErrorCode::download_failed, "cannot read '" + utf8(source) + "'");
    }
}

#if defined(_WIN32)

std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

class Internet {
  public:
    explicit Internet(HINTERNET h) noexcept : h_(h) {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    ~Internet() {
        if (h_ != nullptr) {
            WinHttpCloseHandle(h_);
        }
    }
    HINTERNET get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }

  private:
    HINTERNET h_;
};

[[noreturn]] void winhttp_failed(const char* what) {
    const DWORD code = GetLastError();
    std::string text;
    wchar_t* message = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                       FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   GetModuleHandleW(L"winhttp.dll"), code, 0,
                                   reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    if (n > 0 && message != nullptr) {
        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        text.resize(static_cast<std::size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, message, static_cast<int>(n), text.data(), bytes, nullptr, nullptr);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
    }
    if (message != nullptr) {
        LocalFree(message);
    }
    throw Error(ErrorCode::download_failed, std::string(what) + " failed (WinHTTP error " +
                                                std::to_string(code) +
                                                (text.empty() ? std::string(")") : "): " + text));
}

void fetch_http(const std::string& url, const fs::path& part, std::uint64_t offset, const Transfer& t) {
    const std::wstring wide_url = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        winhttp_failed("parsing the URL");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
    object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    // The system's proxy settings (Windows 8.1 and later), else the WinHTTP default.
    HINTERNET opened = WinHttpOpen(L"DQ8-Native", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (opened == nullptr) {
        opened = WinHttpOpen(L"DQ8-Native", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    }
    const Internet session(opened);
    if (!session) {
        winhttp_failed("opening a WinHTTP session");
    }
    const int stall_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::max(t.options.stall_timeout, std::chrono::seconds(1)))
                                              .count());
    WinHttpSetTimeouts(session.get(), 30000, 30000, 30000, stall_ms);
    const Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) {
        winhttp_failed("connecting");
    }
    const Internet request(WinHttpOpenRequest(connection.get(), L"GET", object.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) {
        winhttp_failed("opening the request");
    }
    std::wstring headers;
    if (offset > 0) {
        headers = L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n";
    }
    if (!WinHttpSendRequest(request.get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        winhttp_failed("sending the request");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        winhttp_failed("receiving the response");
    }
    DWORD status = 0;
    DWORD status_size = sizeof status;
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        winhttp_failed("reading the status");
    }
    if (status == 416) {
        remove_file(part); // our partial file is not a prefix the server knows: start over
        throw Error(ErrorCode::download_failed, "the server refused the resume range (HTTP 416)");
    }
    if (status != 200 && !(status == 206 && offset > 0)) {
        throw Error(ErrorCode::download_failed, "the server answered HTTP " + std::to_string(status));
    }
    const bool append = status == 206;
    std::ofstream out(part, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) {
        throw Error(ErrorCode::io_write, "cannot write '" + utf8(part) + "'");
    }
    std::uint64_t done = append ? offset : 0;
    std::vector<char> buffer(std::size_t{64} << 10);
    for (;;) {
        if (t.cancelled()) {
            throw_cancelled();
        }
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
            out.flush();
            winhttp_failed("reading the response");
        }
        if (got == 0) {
            break;
        }
        out.write(buffer.data(), static_cast<std::streamsize>(got));
        if (!out) {
            throw Error(ErrorCode::io_write, "cannot write '" + utf8(part) + "'");
        }
        out.flush();
        done += got;
        t.progress(done);
    }
}

#else

// The last "curl: (N) ..." line of curl's standard error (the rest is its progress bar).
std::string curl_message(const std::string& err) {
    const std::size_t at = err.rfind("curl: (");
    if (at == std::string::npos) {
        return err.empty() ? std::string("no message")
                           : err.substr(err.size() - std::min<std::size_t>(err.size(), 200));
    }
    const std::size_t end = err.find_first_of("\r\n", at);
    return err.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

void fetch_http(const std::string& url, const fs::path& part, std::uint64_t offset, const Transfer& t) {
    const std::optional<fs::path> curl = find_program(t.options.curl);
    if (!curl) {
        throw Error(ErrorCode::tool_missing,
                    "curl was not found; it downloads the toolchain on this system (install curl)");
    }
    ProcessOptions p;
    p.program = *curl;
    p.args = {"--fail",
              "--location",
              "--show-error",
              "--progress-bar",
              "--connect-timeout",
              "30",
              "--speed-limit",
              "1",
              "--speed-time",
              std::to_string(std::max<long long>(t.options.stall_timeout.count(), 1)),
              "--output",
              utf8(part)};
    if (offset > 0) {
        p.args.push_back("--continue-at");
        p.args.push_back(std::to_string(offset));
    }
    p.args.push_back(url);
    p.cancel = t.options.cancel;
    p.capture_limit = std::size_t{64} << 10;
    // curl's progress bar arrives several times a second: report the file's size each time.
    p.on_output = [&](OutputStream, std::string_view) { t.progress(size_or_zero(part)); };
    const ProcessResult r = run_process(p);
    if (r.cancelled) {
        throw_cancelled();
    }
    if (r.exit_code == 33 && offset > 0) {
        remove_file(part); // the server ignores ranges: start over
        throw Error(ErrorCode::download_failed, "the server cannot resume (" + curl_message(r.err) + ")");
    }
    if (!r.ok()) {
        throw Error(ErrorCode::download_failed, curl_message(r.err));
    }
    t.progress(size_or_zero(part));
}

#endif

void fetch(const std::string& url, const fs::path& part, std::uint64_t offset, const Transfer& t) {
    if (const std::optional<fs::path> local = local_path(url)) {
        fetch_local(*local, part, offset, t);
    } else {
        fetch_http(url, part, offset, t);
    }
}

// Transfers until the part file holds the whole archive. Attempts that end early (a dropped connection) are
// resumed; three attempts in a row without new bytes end the download.
void download_archive(const DownloadOptions& o, const fs::path& part, DownloadResult& result) {
    const DownloadPin& pin = o.pin;
    const Transfer t{o, pin.size};
    int fruitless = 0;
    for (;;) {
        std::uint64_t before = size_or_zero(part);
        if (pin.size != 0 && before > pin.size) {
            remove_file(part);
            before = 0;
        }
        if (pin.size != 0 && before == pin.size) {
            return;
        }
        if (t.cancelled()) {
            throw_cancelled();
        }
        std::optional<Error> failure;
        try {
            fetch(pin.url, part, before, t);
        } catch (const Error& e) {
            if (e.code() != ErrorCode::download_failed) {
                throw;
            }
            failure = e;
        }
        const std::uint64_t after = size_or_zero(part);
        result.downloaded_bytes += after >= before ? after - before : after;
        if (!failure && (pin.size == 0 || after == pin.size)) {
            return;
        }
        fruitless = after > before ? 0 : fruitless + 1;
        if (fruitless >= 3) {
            if (failure) {
                throw Error(ErrorCode::download_failed,
                            "the toolchain download failed: " + std::string(failure->what()));
            }
            throw Error(ErrorCode::download_failed, "the toolchain download stopped at " +
                                                        std::to_string(after) + " of " +
                                                        std::to_string(pin.size) + " bytes");
        }
        if (fruitless > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * fruitless));
        }
    }
}

void verify(const DownloadOptions& o, const fs::path& archive) {
    const std::uint64_t total = size_or_zero(archive);
    const std::string digest = sha256_file(archive, [&](std::uint64_t done) {
        if (o.cancel != nullptr && o.cancel->load()) {
            throw_cancelled();
        }
        if (o.on_progress) {
            o.on_progress({DownloadPhase::verify, done, total});
        }
    });
    if (digest != o.pin.sha256) {
        remove_file(archive);
        throw Error(ErrorCode::download_failed, "the downloaded toolchain failed its checksum (SHA-256 " +
                                                    digest + ", expected " + o.pin.sha256 +
                                                    "); it was deleted");
    }
}

std::string output_tail(const std::string& text) {
    constexpr std::size_t keep = 400;
    return text.size() <= keep ? text : "..." + text.substr(text.size() - keep);
}

void unpack(const DownloadOptions& o, const fs::path& archive, const fs::path& staging) {
    const std::optional<fs::path> cmake = find_program(o.cmake);
    if (!cmake) {
        throw Error(ErrorCode::tool_missing, "cmake was not found; it unpacks the toolchain and ships with "
                                             "DQ8-Native (reinstall it, or put cmake on PATH)");
    }
    remove_tree(staging);
    platform::make_directories(staging);
    ProcessOptions p;
    p.program = *cmake;
    p.args = {"-E", "tar", "xvf", utf8(archive)};
    p.working_dir = staging;
    p.cancel = o.cancel;
    p.capture_limit = std::size_t{256} << 10;
    // `cmake -E tar xv` prints one line per entry.
    std::uint64_t entries = 0;
    p.on_output = [&](OutputStream, std::string_view chunk) {
        entries += static_cast<std::uint64_t>(std::count(chunk.begin(), chunk.end(), '\n'));
        if (o.on_progress) {
            const std::uint64_t total = o.pin.entries;
            o.on_progress({DownloadPhase::unpack, total != 0 ? std::min(entries, total) : entries, total});
        }
    };
    const ProcessResult r = run_process(p);
    if (r.cancelled) {
        throw_cancelled();
    }
    if (!r.ok()) {
        throw Error(ErrorCode::download_failed, "unpacking '" + utf8(archive) + "' failed: " +
                                                    output_tail(r.err.empty() ? r.out : r.err));
    }
}

constexpr const char* marker_name = ".dq8-toolchain";

} // namespace

std::string file_url(const fs::path& file) {
    std::error_code ec;
    fs::path absolute = fs::absolute(file, ec);
    if (ec) {
        absolute = file;
    }
    const std::string path = utf8(absolute.generic_u8string());
    std::string url = "file://";
    if (path.empty() || path.front() != '/') {
        url += '/';
    }
    static constexpr char digits[] = "0123456789ABCDEF";
    for (const char c : path) {
        if (unreserved(c)) {
            url += c;
        } else {
            const auto b = static_cast<unsigned char>(c);
            url += '%';
            url += digits[b >> 4];
            url += digits[b & 0xF];
        }
    }
    return url;
}

std::optional<fs::path> installed_toolchain(const fs::path& bundled_root, const DownloadPin& pin) {
    try {
        const fs::path dir = bundled_root / pin.version;
        std::ifstream in(dir / marker_name);
        std::string version;
        std::string digest;
        if (in >> version >> digest && version == pin.version && digest == pin.sha256) {
            return dir;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

DownloadResult download_toolchain(const DownloadOptions& o) {
    validate(o.pin);
    const fs::path root = o.bundled_root.empty() ? platform::user_data_dir() / "toolchain" : o.bundled_root;
    DownloadResult result;
    if (const std::optional<fs::path> installed = installed_toolchain(root, o.pin)) {
        result.root = *installed;
        result.already_installed = true;
        return result;
    }
    if (!o.consent) {
        throw Error(ErrorCode::download_failed, "the toolchain (llvm-mingw " + o.pin.version +
                                                    ") is downloaded only with the user's consent");
    }

    const fs::path downloads = root / "downloads";
    platform::make_directories(downloads);
    const fs::path archive = downloads / archive_name(o.pin);
    fs::path part = archive;
    part += ".part";

    if (!fs::exists(archive)) {
        result.resumed_from = size_or_zero(part);
        const std::uint64_t remaining =
            o.pin.size > result.resumed_from ? o.pin.size - result.resumed_from : 0;
        check_space(root, remaining + o.pin.unpacked_bytes + 64 * mib);
        download_archive(o, part, result);
        verify(o, part);
        std::error_code ec;
        fs::rename(part, archive, ec);
        if (ec) {
            throw Error(ErrorCode::io_write, "cannot rename '" + utf8(part) + "': " + ec.message());
        }
    } else {
        // Verified by an earlier call that stopped before unpacking; check again (cheap next to the
        // download).
        check_space(root, o.pin.unpacked_bytes + 64 * mib);
        verify(o, archive);
    }

    fs::path staging = root / o.pin.version;
    staging += ".unpack";
    unpack(o, archive, staging);
    const fs::path unpacked = staging / o.pin.archive_root;
    if (!fs::is_directory(unpacked)) {
        remove_tree(staging);
        throw Error(ErrorCode::download_failed,
                    "the toolchain archive does not hold the directory '" + o.pin.archive_root + "'");
    }
    const fs::path target = root / o.pin.version;
    remove_tree(target); // an incomplete earlier attempt (no marker)
    rename_into_place(unpacked, target);
    remove_tree(staging);
    platform::write_file_atomic(target / marker_name, o.pin.version + " " + o.pin.sha256 + "\n");
    if (!o.keep_archive) {
        remove_file(archive);
    }
    result.root = target;
    return result;
}

} // namespace dq8::core::toolchain
