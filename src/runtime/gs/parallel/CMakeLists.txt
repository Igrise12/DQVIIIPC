# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# dq8-gs-parallel: our C ABI wrapper (GPL-3.0-or-later) + paraLLEl-GS (LGPL-3.0-or-later) + Granite's vulkan, util,
# math and volk archives (MIT), as one shared library that exports only dq8_gsp_* (ADR-0004). dq8-game loads it at
# run time (runtime/gs/gs_parallel_backend.h), so the rest of the host never links Granite or paraLLEl-GS and a user
# can rebuild and replace this one file (NOTICE.md says how). The file is named like the game modules:
# no "lib" prefix (rt::shared_library_file_name("dq8-gs-parallel")).
add_library(dq8-gs-parallel SHARED gs_parallel.cpp)
target_include_directories(dq8-gs-parallel PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(dq8-gs-parallel PRIVATE cxx_std_20)
target_link_libraries(dq8-gs-parallel PRIVATE parallel-gs)
set_target_properties(dq8-gs-parallel PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
dq8_set_warnings(dq8-gs-parallel)
dq8_enable_sanitizers(dq8-gs-parallel)
if(NOT MSVC)
    # paraLLEl-GS's headers are C++14 and use std::is_pod, which C++20 deprecates (S5a open question).
    target_compile_options(dq8-gs-parallel PRIVATE -Wno-deprecated-declarations)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(_dq8_gsp_map "${CMAKE_CURRENT_SOURCE_DIR}/dq8_gsp.map")
    target_link_options(dq8-gs-parallel PRIVATE "LINKER:--version-script=${_dq8_gsp_map}")
    set_property(TARGET dq8-gs-parallel APPEND PROPERTY LINK_DEPENDS "${_dq8_gsp_map}")
    # Every symbol must resolve inside the library or its declared dependencies. Not with sanitizers: their
    # runtime is only resolved when an instrumented executable loads the library.
    if(NOT DQ8_SANITIZERS)
        target_link_options(dq8-gs-parallel PRIVATE "LINKER:--no-undefined")
    endif()
endif()

# The LGPL text and our notice travel with the library, in the build tree and in an install.
add_custom_command(TARGET dq8-gs-parallel POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/third_party/parallel-gs/COPYING.LGPLv3"
        "$<TARGET_FILE_DIR:dq8-gs-parallel>/dq8-gs-parallel.COPYING.LGPLv3"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
        "$<TARGET_FILE_DIR:dq8-gs-parallel>/dq8-gs-parallel.NOTICE.md"
    VERBATIM)

include(GNUInstallDirs)
install(TARGETS dq8-gs-parallel
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT game
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT game)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/parallel-gs/COPYING.LGPLv3"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/dq8-gs-parallel" COMPONENT game)
install(FILES NOTICE.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/dq8-gs-parallel" COMPONENT game)
