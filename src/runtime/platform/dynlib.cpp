// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "runtime/platform/dynlib.h"

#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstring>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dq8::rt {

DynamicLibrary DynamicLibrary::open(const std::filesystem::path& file) {
#if defined(_WIN32)
    HMODULE h = ::LoadLibraryW(file.wstring().c_str());
    if (h == nullptr) {
        throw DynlibError("cannot load " + file.string() + ": LoadLibrary failed with error " +
                          std::to_string(::GetLastError()));
    }
    return DynamicLibrary(static_cast<void*>(h));
#else
    void* h = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        const char* why = ::dlerror();
        throw DynlibError("cannot load " + file.string() + ": " + (why != nullptr ? why : "dlopen failed"));
    }
    return DynamicLibrary(h);
#endif
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() {
    close();
}

void DynamicLibrary::close() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (handle_ == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    const FARPROC p = ::GetProcAddress(static_cast<HMODULE>(handle_), name);
    void* out = nullptr;
    static_assert(sizeof out == sizeof p);
    std::memcpy(&out, &p, sizeof out);
    return out;
#else
    return ::dlsym(handle_, name);
#endif
}

std::string shared_library_file_name(const std::string& name) {
#if defined(_WIN32)
    return name + ".dll";
#elif defined(__APPLE__)
    return "lib" + name + ".dylib";
#else
    return "lib" + name + ".so";
#endif
}

} // namespace dq8::rt
