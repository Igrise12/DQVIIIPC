# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# The install layout (Docs/milestones/M5-launcher.md, session D). Included from the top-level CMakeLists.txt
# before the targets, so the RPATH applies to every one of them. `cmake --install <build> --prefix <dir>` gives:
#
#   bin/        dq8-game, dq8-launcher, dq8ctl, ps2_recomp
#   lib/        dq8-gs-parallel, dq8-movie and, with DQ8_FFMPEG=bundled, libavcodec and libavutil
#   share/dq8/  regions/ (region profiles), maps/ (published function maps), settings/ (schema and strings),
#               cmake/module/ (the game-module template), include/dq8/abi/ (the headers modules compile against),
#               fonts/ (when the launcher bundles one)
#   share/doc/dq8-native/  LICENSE, README.md, THIRD_PARTY_NOTICES.md, licenses/, the libraries' notices
#
# The programs find share/dq8 from bin/ (find_maps_dir, find_settings_dir, dq8ctl's data search all look under
# share/dq8 beside each ancestor of the executable) and the libraries in lib/ through the RPATH and their own
# ../lib search (default_parallel_gs_library, default_movie_library). The tree is relocatable.
include(GNUInstallDirs)

# Executables in bin/ and libraries in lib/ both reach lib/ as $ORIGIN/../lib.
if(NOT APPLE AND NOT WIN32)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
set(DQ8_INSTALL_DATADIR "${CMAKE_INSTALL_DATADIR}/dq8")

install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/regions/" DESTINATION "${DQ8_INSTALL_DATADIR}/regions"
        COMPONENT game FILES_MATCHING PATTERN "*.toml")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/maps/" DESTINATION "${DQ8_INSTALL_DATADIR}/maps"
        COMPONENT recompiler FILES_MATCHING PATTERN "*.toml")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/config/settings/" DESTINATION "${DQ8_INSTALL_DATADIR}/settings"
        COMPONENT game FILES_MATCHING PATTERN "*.toml")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/cmake/module/" DESTINATION "${DQ8_INSTALL_DATADIR}/cmake/module"
        COMPONENT recompiler)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/dq8/abi/" DESTINATION "${DQ8_INSTALL_DATADIR}/include/dq8/abi"
        COMPONENT recompiler PATTERN ".clang-format-ignore" EXCLUDE)
if(IS_DIRECTORY "${PROJECT_SOURCE_DIR}/data/fonts")
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/fonts/" DESTINATION "${DQ8_INSTALL_DATADIR}/fonts"
            COMPONENT launcher)
endif()
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/README.md"
              "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT game)

# Licence texts of the libraries linked into the programs (ps2_recomp's own are installed by cmake/Ps2Recomp.cmake,
# the GS and movie libraries' by their CMakeLists, FFmpeg's by cmake/Ffmpeg.cmake).
foreach(_license IN ITEMS
        toml11/LICENSE=toml11-LICENSE
        SDL/LICENSE.txt=SDL-LICENSE.txt
        minicoro/LICENSE=minicoro-LICENSE
        stb/LICENSE=stb-LICENSE
        imgui/LICENSE.txt=imgui-LICENSE.txt
        nlohmann_json/LICENSE.MIT=nlohmann_json-LICENSE.MIT)
    string(REPLACE "=" ";" _license "${_license}")
    list(GET _license 0 _from)
    list(GET _license 1 _to)
    if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/${_from}")
        install(FILES "${PROJECT_SOURCE_DIR}/third_party/${_from}" RENAME "${_to}"
                DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" COMPONENT game)
    endif()
endforeach()
