// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "platform/system_info.h"

#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <unistd.h>
#else
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace dq8::core::platform {

unsigned hardware_threads() noexcept {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : n;
}

std::optional<std::uint64_t> available_memory_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status) == 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(status.ullAvailPhys);
#elif defined(__APPLE__)
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats),
                          &count) != KERN_SUCCESS) {
        return std::nullopt;
    }
    const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    return (static_cast<std::uint64_t>(stats.free_count) + stats.inactive_count) * page;
#else
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream fields(line.substr(13));
            std::uint64_t kib = 0;
            if (fields >> kib) {
                return kib * 1024;
            }
        }
    }
    return std::nullopt;
#endif
}

} // namespace dq8::core::platform
