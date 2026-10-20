// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

// A loaded shared library: dlopen(RTLD_NOW | RTLD_LOCAL) on POSIX, LoadLibraryW on Windows. Move-only; the
// library is unloaded when the last owner goes away.
namespace dq8::rt {

class DynlibError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class DynamicLibrary {
  public:
    DynamicLibrary() = default;
    // Throws DynlibError with the loader's message.
    static DynamicLibrary open(const std::filesystem::path& file);

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    ~DynamicLibrary();

    bool loaded() const noexcept { return handle_ != nullptr; }
    // The address of an exported symbol, or null.
    void* symbol(const char* name) const noexcept;

  private:
    explicit DynamicLibrary(void* handle) noexcept : handle_(handle) {}
    void close() noexcept;

    void* handle_ = nullptr;
};

// The platform's file name for a shared library called `name`: "lib<name>.so", "lib<name>.dylib" or
// "<name>.dll", the names CMake gives a SHARED library target.
std::string shared_library_file_name(const std::string& name);

} // namespace dq8::rt
