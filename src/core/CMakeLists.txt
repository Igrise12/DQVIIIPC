# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# libdq8core: platform-neutral foundation (configuration, verification, setup orchestration).
# Knows nothing about the PS2 beyond file formats and nothing about any UI. See src/core/README.md and
# architecture.md §13.
add_library(dq8_core STATIC
    version.cpp
    error.cpp
    hash/hash.cpp
    iso/iso9660.cpp
)
add_library(dq8::core ALIAS dq8_core)
target_include_directories(dq8_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(dq8_core PUBLIC cxx_std_20)
target_compile_definitions(dq8_core PRIVATE DQ8_VERSION="${PROJECT_VERSION}")
target_link_libraries(dq8_core PRIVATE toml11::toml11)
dq8_set_warnings(dq8_core)
dq8_enable_sanitizers(dq8_core)
