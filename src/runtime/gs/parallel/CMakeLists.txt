# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-gs-parallel: our C ABI wrapper (GPL-3.0-or-later) + paraLLEl-GS (LGPL-3.0-or-later) + Granite's vulkan, util,
# math and volk archives (MIT), as one shared library that exports only dq8_gsp_* (ADR-0004). dq8-game loads it at
# run time (runtime/gs/gs_parallel_backend.h), so the rest of the host never links Granite or paraLLEl-GS and a user
# can rebuild and replace this one file (NOTICE.md says how). Target dq8_gs_parallel, file dq8-gs-parallel with the
# platform's prefix and suffix (libdq8-gs-parallel.so, dq8-gs-parallel.dll), M3 contract decision 1.
add_library(dq8_gs_parallel SHARED gs_parallel.cpp)
target_include_directories(dq8_gs_parallel PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(dq8_gs_parallel PRIVATE cxx_std_20)
target_link_libraries(dq8_gs_parallel PRIVATE parallel-gs)
# Built next to dq8-game, where the host looks by default (decision 1): LIBRARY for .so/.dylib, RUNTIME for the
# .dll. The generator expression follows the executable wherever the host puts it, per configuration.
set_target_properties(dq8_gs_parallel PROPERTIES
    OUTPUT_NAME dq8-gs-parallel
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:dq8-game>"
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:dq8-game>")
dq8_set_warnings(dq8_gs_parallel)
dq8_enable_sanitizers(dq8_gs_parallel)
if(NOT MSVC)
    # paraLLEl-GS's headers are C++14 and use std::is_pod, which C++20 deprecates (S5a open question).
    target_compile_options(dq8_gs_parallel PRIVATE -Wno-deprecated-declarations)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(_dq8_gsp_map "${CMAKE_CURRENT_SOURCE_DIR}/dq8_gsp.map")
    target_link_options(dq8_gs_parallel PRIVATE "LINKER:--version-script=${_dq8_gsp_map}")
    set_property(TARGET dq8_gs_parallel APPEND PROPERTY LINK_DEPENDS "${_dq8_gsp_map}")
    # Every symbol must resolve inside the library or its declared dependencies. Not with sanitizers: their
    # runtime is only resolved when an instrumented executable loads the library.
    if(NOT DQ8_SANITIZERS)
        target_link_options(dq8_gs_parallel PRIVATE "LINKER:--no-undefined")
    endif()
endif()

# The LGPL text and our notice travel with the library, in the build tree and in an install.
add_custom_command(TARGET dq8_gs_parallel POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/third_party/parallel-gs/COPYING.LGPLv3"
        "$<TARGET_FILE_DIR:dq8_gs_parallel>/dq8-gs-parallel.COPYING.LGPLv3"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        "$<TARGET_FILE_DIR:dq8_gs_parallel>/dq8-gs-parallel.NOTICE.md"
    VERBATIM)

include(GNUInstallDirs)
install(TARGETS dq8_gs_parallel
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT game
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT game)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/parallel-gs/COPYING.LGPLv3"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/dq8-gs-parallel" COMPONENT game)
install(FILES NOTICE.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/dq8-gs-parallel" COMPONENT game)
