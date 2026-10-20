// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "dq8/core/process.h"

#include "dq8/core/error.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace dq8::core {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

// How long a stopped tree gets between the polite and the forceful stop, and how long output may keep
// flowing after the direct child exited (a grandchild holding the pipes) before the tree is stopped.
constexpr auto stop_grace = 2s;
constexpr auto drain_grace = 1s;
constexpr auto poll_slice = 20ms;
constexpr std::size_t read_chunk = 64u << 10;

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool cancel_requested(const ProcessOptions& o) {
    return o.cancel != nullptr && o.cancel->load();
}

void deliver(const ProcessOptions& o, ProcessResult& r, OutputStream stream, std::string_view chunk) {
    std::string& text = stream == OutputStream::out ? r.out : r.err;
    bool& truncated = stream == OutputStream::out ? r.out_truncated : r.err_truncated;
    const std::size_t room = text.size() < o.capture_limit ? o.capture_limit - text.size() : 0;
    if (chunk.size() > room) {
        truncated = true;
        text.append(chunk.substr(0, room));
    } else {
        text.append(chunk);
    }
    if (o.on_output) {
        o.on_output(stream, chunk);
    }
}

bool is_executable_file(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        return false;
    }
#if defined(_WIN32)
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

} // namespace

std::string quote_windows_argument(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(arg);
    }
    // Backslashes are literal unless they precede a quote; then each one must be doubled and the quote
    // escaped. A run at the very end is doubled because the closing quote follows it.
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
        } else {
            out.append(backslashes, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string describe_command(const fs::path& program, const std::vector<std::string>& args) {
    auto quote = [](std::string_view s) {
        const bool plain = !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   std::strchr("_@%+=:,./-", c) != nullptr;
        });
        if (plain) {
            return std::string(s);
        }
        std::string out = "'";
        for (const char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    };
    std::string line = quote(utf8(program));
    for (const std::string& a : args) {
        line.push_back(' ');
        line += quote(a);
    }
    return line;
}

std::optional<fs::path> find_program(const fs::path& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    std::vector<fs::path::string_type> extensions{fs::path::string_type()};
#if defined(_WIN32)
    if (!name.has_extension()) {
        const wchar_t* pathext = _wgetenv(L"PATHEXT");
        std::wstring list = pathext != nullptr ? pathext : L".COM;.EXE;.BAT;.CMD";
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t end = std::min(list.find(L';', start), list.size());
            if (end > start) {
                extensions.push_back(list.substr(start, end - start));
            }
            start = end + 1;
        }
    }
#endif
    auto try_candidates = [&](const fs::path& base) -> std::optional<fs::path> {
        for (const auto& ext : extensions) {
            fs::path candidate = base;
            candidate += ext;
            if (is_executable_file(candidate)) {
                std::error_code ec;
                const fs::path absolute = fs::absolute(candidate, ec);
                return ec ? candidate : absolute;
            }
        }
        return std::nullopt;
    };
    if (name.has_parent_path() || name.is_absolute()) {
        return try_candidates(name);
    }
#if defined(_WIN32)
    const wchar_t* path_env = _wgetenv(L"PATH");
    const std::wstring path_list = path_env != nullptr ? path_env : L"";
    constexpr wchar_t separator = L';';
#else
    const char* path_env = std::getenv("PATH");
    const std::string path_list = path_env != nullptr ? path_env : "";
    constexpr char separator = ':';
#endif
    std::size_t start = 0;
    while (start <= path_list.size()) {
        const std::size_t end = std::min(path_list.find(separator, start), path_list.size());
        const fs::path dir = end > start ? fs::path(path_list.substr(start, end - start)) : fs::path(".");
        if (auto found = try_candidates(dir / name)) {
            return found;
        }
        start = end + 1;
    }
    return std::nullopt;
}

#if defined(_WIN32)

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string system_message(DWORD code) {
    char* buffer = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string text = n != 0 && buffer != nullptr ? std::string(buffer, n) : "error " + std::to_string(code);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    HANDLE get() const noexcept { return h_; }
    HANDLE* out() noexcept { return &h_; }
    void reset() noexcept {
        if (h_ != nullptr) {
            CloseHandle(h_);
            h_ = nullptr;
        }
    }

  private:
    HANDLE h_ = nullptr;
};

// The environment block for CreateProcessW: the caller's variables with `overrides` applied, sorted
// case-insensitively, each NUL-terminated, the block ending in an extra NUL.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::wstring> vars;
    if (LPWCH base = GetEnvironmentStringsW(); base != nullptr) {
        for (LPWCH p = base; *p != L'\0'; p += std::wcslen(p) + 1) {
            vars.emplace_back(p);
        }
        FreeEnvironmentStringsW(base);
    }
    auto name_of = [](const std::wstring& v) { return v.substr(0, v.find(L'=', 1)); };
    for (const auto& [key, value] : overrides) {
        const std::wstring wkey = widen(key);
        std::erase_if(vars,
                      [&](const std::wstring& v) { return _wcsicmp(name_of(v).c_str(), wkey.c_str()) == 0; });
        vars.push_back(wkey + L"=" + widen(value));
    }
    std::sort(vars.begin(), vars.end(), [&](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(name_of(a).c_str(), name_of(b).c_str()) < 0;
    });
    std::wstring block;
    for (const std::wstring& v : vars) {
        block += v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void make_pipe(Handle& read_end, Handle& write_end) {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(read_end.out(), write_end.out(), &sa, 0)) {
        throw Error(ErrorCode::process_spawn, "cannot create a pipe: " + system_message(GetLastError()));
    }
    SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);
}

} // namespace

ProcessResult run_process(const ProcessOptions& o) {
    const std::optional<fs::path> program = find_program(o.program);
    if (!program) {
        throw Error(ErrorCode::process_spawn, "cannot find the program '" + utf8(o.program) + "'");
    }
    std::error_code ec;
    if (!o.working_dir.empty() && !fs::is_directory(o.working_dir, ec)) {
        throw Error(ErrorCode::process_spawn,
                    "working directory '" + utf8(o.working_dir) + "' does not exist");
    }
    std::string command_line = quote_windows_argument(utf8(*program));
    for (const std::string& a : o.args) {
        command_line.push_back(' ');
        command_line += quote_windows_argument(a);
    }
    std::wstring wide_command = widen(command_line);
    std::wstring env = o.env.empty() ? std::wstring() : environment_block(o.env);
    const std::wstring cwd = o.working_dir.empty() ? std::wstring() : fs::absolute(o.working_dir).wstring();

    Handle out_r, out_w, err_r, err_w;
    make_pipe(out_r, out_w);
    make_pipe(err_r, err_w);
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                           nullptr));
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.get() != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul.get();
    si.hStdOutput = out_w.get();
    si.hStdError = err_w.get();
    PROCESS_INFORMATION pi{};
    const std::wstring program_w = program->wstring();
    const DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    if (!CreateProcessW(program_w.c_str(), wide_command.data(), nullptr, nullptr, TRUE, flags,
                        env.empty() ? nullptr : env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        throw Error(ErrorCode::process_spawn,
                    "cannot start '" + utf8(*program) + "': " + system_message(GetLastError()));
    }
    Handle process(pi.hProcess);
    Handle thread(pi.hThread);
    const bool in_job = job.get() != nullptr && AssignProcessToJobObject(job.get(), process.get());
    ResumeThread(thread.get());
    thread.reset();
    out_w.reset();
    err_w.reset();
    nul.reset();

    struct Chunk {
        OutputStream stream;
        std::string data;
    };
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Chunk> queue;
    int open_readers = 2;
    auto reader = [&](HANDLE h, OutputStream stream) {
        std::string buffer(read_chunk, '\0');
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) || n == 0) {
                break;
            }
            {
                std::lock_guard lock(mutex);
                queue.push_back({stream, buffer.substr(0, n)});
            }
            ready.notify_one();
        }
        {
            std::lock_guard lock(mutex);
            --open_readers;
        }
        ready.notify_one();
    };
    std::thread out_thread(reader, out_r.get(), OutputStream::out);
    std::thread err_thread(reader, err_r.get(), OutputStream::err);

    ProcessResult result;
    const auto start = Clock::now();
    const auto deadline = o.timeout.count() > 0 ? start + o.timeout : Clock::time_point::max();
    bool exited = false;
    bool stopped = false;
    auto stop_tree = [&] {
        if (in_job) {
            TerminateJobObject(job.get(), 1);
        } else {
            TerminateProcess(process.get(), 1);
        }
        stopped = true;
    };
    std::optional<Clock::time_point> drain_deadline;
    std::optional<Clock::time_point> cancel_io_at;
    try {
        for (;;) {
            std::deque<Chunk> chunks;
            bool readers_done = false;
            {
                std::unique_lock lock(mutex);
                ready.wait_for(lock, poll_slice, [&] { return !queue.empty() || open_readers == 0; });
                chunks.swap(queue);
                readers_done = open_readers == 0;
            }
            for (const Chunk& c : chunks) {
                deliver(o, result, c.stream, c.data);
            }
            const auto now = Clock::now();
            if (!exited && WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                exited = true;
                drain_deadline = now + drain_grace;
            }
            if (exited && readers_done) {
                break;
            }
            if (!exited && !stopped) {
                if (cancel_requested(o)) {
                    result.cancelled = true;
                } else if (now >= deadline) {
                    result.timed_out = true;
                }
                if (result.cancelled || result.timed_out) {
                    stop_tree();
                }
            }
            if (exited && !stopped && drain_deadline && now >= *drain_deadline) {
                stop_tree(); // a grandchild still holds the pipes
            }
            if (stopped && !cancel_io_at) {
                cancel_io_at = now + stop_grace;
            }
            if (cancel_io_at && now >= *cancel_io_at) {
                // Last resort: a process outside the job holds the pipes; abandon the reads.
                CancelSynchronousIo(out_thread.native_handle());
                CancelSynchronousIo(err_thread.native_handle());
            }
        }
    } catch (...) {
        stop_tree();
        CancelSynchronousIo(out_thread.native_handle());
        CancelSynchronousIo(err_thread.native_handle());
        out_thread.join();
        err_thread.join();
        throw;
    }
    out_thread.join();
    err_thread.join();
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    result.exit_code = (result.timed_out || result.cancelled) ? -1 : static_cast<int>(code);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return result;
}

#else // POSIX

namespace {

class Fd {
  public:
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    int get() const noexcept { return fd_; }
    void set(int fd) noexcept {
        reset();
        fd_ = fd;
    }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_ = -1;
};

std::string errno_message(int code) {
    return std::strerror(code);
}

void make_pipe(Fd& read_end, Fd& write_end) {
    int fds[2] = {-1, -1};
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        throw Error(ErrorCode::process_spawn, "cannot create a pipe: " + errno_message(errno));
    }
#else
    if (::pipe(fds) != 0) {
        throw Error(ErrorCode::process_spawn, "cannot create a pipe: " + errno_message(errno));
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    read_end.set(fds[0]);
    write_end.set(fds[1]);
    ::fcntl(read_end.get(), F_SETFL, ::fcntl(read_end.get(), F_GETFL) | O_NONBLOCK);
}

// RAII for the posix_spawn attribute objects.
struct SpawnSetup {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attr{};
    SpawnSetup() {
        posix_spawn_file_actions_init(&actions);
        posix_spawnattr_init(&attr);
    }
    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
    ~SpawnSetup() {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
    }
};

} // namespace

ProcessResult run_process(const ProcessOptions& o) {
    const std::optional<fs::path> program = find_program(o.program);
    if (!program) {
        throw Error(ErrorCode::process_spawn, "cannot find the program '" + utf8(o.program) + "'");
    }
    std::error_code ec;
    if (!o.working_dir.empty() && !fs::is_directory(o.working_dir, ec)) {
        throw Error(ErrorCode::process_spawn,
                    "working directory '" + utf8(o.working_dir) + "' does not exist");
    }

    std::vector<std::string> argv_storage{o.program.string()};
    argv_storage.insert(argv_storage.end(), o.args.begin(), o.args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_storage) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string_view entry(*e);
        const std::string_view key = entry.substr(0, entry.find('='));
        const bool overridden =
            std::any_of(o.env.begin(), o.env.end(), [&](const auto& kv) { return kv.first == key; });
        if (!overridden) {
            env_storage.emplace_back(entry);
        }
    }
    for (const auto& [key, value] : o.env) {
        env_storage.push_back(key + "=" + value);
    }
    std::vector<char*> envp;
    for (std::string& e : env_storage) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    Fd out_r, out_w, err_r, err_w;
    make_pipe(out_r, out_w);
    make_pipe(err_r, err_w);

    SpawnSetup setup;
    posix_spawn_file_actions_addopen(&setup.actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&setup.actions, out_w.get(), 1);
    posix_spawn_file_actions_adddup2(&setup.actions, err_w.get(), 2);
    std::string cwd;
    if (!o.working_dir.empty()) {
        cwd = fs::absolute(o.working_dir).string();
        posix_spawn_file_actions_addchdir_np(&setup.actions, cwd.c_str());
    }
    // The child leads its own process group so a stop reaches everything it starts; it gets default signal
    // dispositions and an empty mask whatever ours are.
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int sig : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD}) {
        sigaddset(&defaults, sig);
    }
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    posix_spawnattr_setsigdefault(&setup.attr, &defaults);
    posix_spawnattr_setsigmask(&setup.attr, &empty_mask);
    posix_spawnattr_setpgroup(&setup.attr, 0);
    posix_spawnattr_setflags(&setup.attr, static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                                                             POSIX_SPAWN_SETSIGMASK));

    pid_t pid = -1;
    const std::string path = program->string();
    const int spawn_error =
        posix_spawn(&pid, path.c_str(), &setup.actions, &setup.attr, argv.data(), envp.data());
    if (spawn_error != 0) {
        throw Error(ErrorCode::process_spawn, "cannot start '" + path + "': " + errno_message(spawn_error));
    }
    out_w.reset();
    err_w.reset();

    ProcessResult result;
    const auto start = Clock::now();
    const auto deadline = o.timeout.count() > 0 ? start + o.timeout : Clock::time_point::max();
    bool reaped = false;
    int status = 0;
    bool stopping = false;
    bool killed = false;
    Clock::time_point kill_at{};
    std::optional<Clock::time_point> drain_deadline;
    std::string buffer(read_chunk, '\0');
    Fd* const fds[2] = {&out_r, &err_r};
    const OutputStream streams[2] = {OutputStream::out, OutputStream::err};

    auto drain = [&](std::size_t i) {
        for (;;) {
            const ssize_t n = ::read(fds[i]->get(), buffer.data(), buffer.size());
            if (n > 0) {
                deliver(o, result, streams[i], std::string_view(buffer.data(), static_cast<std::size_t>(n)));
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            fds[i]->reset(); // EOF or a read error
            return;
        }
    };

    try {
        while (out_r.get() >= 0 || err_r.get() >= 0 || !reaped) {
            auto now = Clock::now();
            if (!reaped && !stopping) {
                if (cancel_requested(o)) {
                    result.cancelled = true;
                } else if (now >= deadline) {
                    result.timed_out = true;
                }
                if (result.cancelled || result.timed_out) {
                    ::kill(-pid, SIGTERM);
                    stopping = true;
                    kill_at = now + stop_grace;
                }
            }
            if (stopping && !killed && !reaped && now >= kill_at) {
                ::kill(-pid, SIGKILL);
                killed = true;
            }

            pollfd pfds[2];
            std::size_t which[2] = {0, 0};
            nfds_t n = 0;
            for (std::size_t i = 0; i < 2; ++i) {
                if (fds[i]->get() >= 0) {
                    pfds[n] = {fds[i]->get(), POLLIN, 0};
                    which[n] = i;
                    ++n;
                }
            }
            const int slice = static_cast<int>(poll_slice.count());
            const int rc = ::poll(n > 0 ? pfds : nullptr, n, slice);
            if (rc < 0 && errno != EINTR) {
                throw Error(ErrorCode::io_read, "poll failed: " + errno_message(errno));
            }
            for (nfds_t k = 0; rc > 0 && k < n; ++k) {
                if ((pfds[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
                    drain(which[k]);
                }
            }

            now = Clock::now();
            if (!reaped) {
                const pid_t w = ::waitpid(pid, &status, WNOHANG);
                if (w == pid || (w < 0 && errno == ECHILD)) {
                    reaped = true;
                    drain_deadline = now + drain_grace;
                }
            } else if ((out_r.get() >= 0 || err_r.get() >= 0) && drain_deadline && now >= *drain_deadline) {
                // The child exited but something it started still holds the pipes: stop the group and
                // stop reading.
                ::kill(-pid, SIGKILL);
                out_r.reset();
                err_r.reset();
            }
        }
    } catch (...) {
        if (!reaped) {
            ::kill(-pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
        throw;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
        result.exit_code = -1;
    }
    if (result.timed_out || result.cancelled) {
        result.exit_code = -1;
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return result;
}

#endif

} // namespace dq8::core
