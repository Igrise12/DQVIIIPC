# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Pinned third-party dependencies (git submodules). Each one is listed in THIRD_PARTY_NOTICES.md.
if(DQ8_USE_SYSTEM_TOML11)
    find_package(toml11 CONFIG REQUIRED)
else()
    add_subdirectory(toml11 EXCLUDE_FROM_ALL)
    # Our warning set is stricter than toml11's; treat its headers as system headers so they never trip
    # DQ8_WERROR (clang 20+ warns about its literal operators, for instance).
    get_target_property(_dq8_toml11_includes toml11 INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(toml11 PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dq8_toml11_includes}")
endif()

if(DQ8_BUILD_TESTS)
    if(DQ8_USE_SYSTEM_CATCH2)
        find_package(Catch2 3 CONFIG REQUIRED)
    else()
        add_subdirectory(Catch2 EXCLUDE_FROM_ALL)
        list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Catch2/extras")
        set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
    endif()
endif()

# Header-only dependencies as INTERFACE targets with system include directories (no warnings from their code).
# minicoro's implementation is compiled in exactly one translation unit of ours (MINICORO_IMPL, ADR-0007).
add_library(dq8_minicoro INTERFACE)
add_library(dq8::minicoro ALIAS dq8_minicoro)
target_include_directories(dq8_minicoro SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/minicoro")
# stb: only stb_image_write.h is used, compiled in one translation unit (STB_IMAGE_WRITE_IMPLEMENTATION).
add_library(dq8_stb INTERFACE)
add_library(dq8::stb ALIAS dq8_stb)
target_include_directories(dq8_stb SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stb")
# nlohmann/json: the launcher reads dq8-game's and the setup's JSON-line events with it (M5), header-only.
add_library(dq8_nlohmann_json INTERFACE)
add_library(dq8::nlohmann_json ALIAS dq8_nlohmann_json)
target_include_directories(dq8_nlohmann_json SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/single_include")
# SDL3 (DQ8_BUILD_WINDOW) and paraLLEl-GS (DQ8_BUILD_GS_PARALLEL) are wired up in cmake/Sdl3.cmake and
# cmake/ParallelGs.cmake (M3 session D).
if(DQ8_BUILD_WINDOW)
    include(Sdl3)
endif()
if(DQ8_BUILD_GS_PARALLEL)
    include(ParallelGs)
endif()
# Dear ImGui with its SDL3 platform and SDL renderer backends (ADR-0010), for dq8-launcher only. A static library
# without our warning set or sanitizers; its headers are system headers for our code.
if(DQ8_BUILD_LAUNCHER)
    set(_dq8_imgui_dir "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
    add_library(dq8_imgui STATIC
        "${_dq8_imgui_dir}/imgui.cpp"
        "${_dq8_imgui_dir}/imgui_draw.cpp"
        "${_dq8_imgui_dir}/imgui_tables.cpp"
        "${_dq8_imgui_dir}/imgui_widgets.cpp"
        "${_dq8_imgui_dir}/backends/imgui_impl_sdl3.cpp"
        "${_dq8_imgui_dir}/backends/imgui_impl_sdlrenderer3.cpp"
    )
    add_library(dq8::imgui ALIAS dq8_imgui)
    target_include_directories(dq8_imgui SYSTEM PUBLIC "${_dq8_imgui_dir}" "${_dq8_imgui_dir}/backends")
    # imgui_demo.cpp is not built (the demo and style-editor windows are not shipped).
    target_compile_definitions(dq8_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(dq8_imgui PUBLIC SDL3::SDL3)
    set_target_properties(dq8_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
