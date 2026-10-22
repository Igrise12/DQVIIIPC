# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# M5 session E: the toolchain download (test_toolchain_download*) and Windows-specific paths and processes.
target_sources(dq8_unit_tests PRIVATE
    test_toolchain_download.cpp
    test_windows_platform.cpp
    support/fake_http_server.cpp)
if(WIN32)
    target_link_libraries(dq8_unit_tests PRIVATE ws2_32)
endif()
# The cmake that unpacks the test archives: this one on a native build. A cross build to Windows needs a Windows
# cmake.exe (packaging/windows/fetch_tools.cmake unpacks the pinned one); without it those tests are skipped.
if(CMAKE_CROSSCOMPILING)
    set(DQ8_TEST_WINDOWS_CMAKE "" CACHE FILEPATH "A Windows cmake.exe for the download tests of a cross build")
    set(_dq8_test_cmake "${DQ8_TEST_WINDOWS_CMAKE}")
else()
    set(_dq8_test_cmake "${CMAKE_COMMAND}")
endif()
set_property(SOURCE test_toolchain_download.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             "DQ8_TEST_CMAKE=\"${_dq8_test_cmake}\"")
