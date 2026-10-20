# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Script mode (cmake -P), the download step of the dq8_ps2recomp external project (cmake/Ps2Recomp.cmake):
# replace SOURCE_DIR with a fresh copy of the pinned PS2Recomp tree, apply patches/ps2recomp/*.patch in name
# order, and empty BINARY_DIR so nothing compiled from the previous snapshot survives.
#
# Inputs: -DUPSTREAM_DIR=<submodule> -DSOURCE_DIR=<copy> -DBINARY_DIR=<build tree> -DPATCH_DIR=<queue>
#         -DGIT_EXECUTABLE=<git> (needed only when the queue is not empty). -DPATCH_SET_ID is not read here: it
#         is on the command line so that adding or removing a patch changes the command and re-runs this step.
cmake_minimum_required(VERSION 3.25)

foreach(var UPSTREAM_DIR SOURCE_DIR BINARY_DIR PATCH_DIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "Ps2RecompPrepare: ${var} is not set")
    endif()
endforeach()
if(NOT EXISTS "${UPSTREAM_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Ps2RecompPrepare: ${UPSTREAM_DIR} is empty; git submodule update --init --recursive")
endif()

file(REMOVE_RECURSE "${SOURCE_DIR}" "${BINARY_DIR}")
file(MAKE_DIRECTORY "${SOURCE_DIR}" "${BINARY_DIR}")
# The submodule's .git is a gitlink into this repository's object store; in a copy it would let git commands
# act on the real submodule checkout.
file(COPY "${UPSTREAM_DIR}/" DESTINATION "${SOURCE_DIR}" PATTERN ".git" EXCLUDE)

file(GLOB patches LIST_DIRECTORIES false "${PATCH_DIR}/*.patch")
list(SORT patches)
if(NOT patches)
    message(STATUS "PS2Recomp: the patch queue is empty; building the pinned commit as is")
    return()
endif()
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "Ps2RecompPrepare: the patch queue needs git (GIT_EXECUTABLE)")
endif()

# GIT_CEILING_DIRECTORIES keeps git from finding this repository above the copy: inside a work tree, git apply
# resolves paths from the top of that tree and silently skips those outside the current directory.
get_filename_component(ceiling "${SOURCE_DIR}" DIRECTORY)
foreach(patch IN LISTS patches)
    get_filename_component(name "${patch}" NAME)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${ceiling}"
                "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "PS2Recomp: ${name} does not apply to the pinned tree:\n${out}")
    endif()
    message(STATUS "PS2Recomp: applied ${name}")
endforeach()
