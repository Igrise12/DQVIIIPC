# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
# Writes a C++ source that holds a file's bytes, for the launcher's bundled font:
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<name> -P embed_file.cmake
# The source defines `extern const unsigned char SYMBOL[]` and `extern const std::size_t SYMBOL_size` in
# namespace dq8::launcher. Only our own assets go through this (never game data).
foreach(var INPUT OUTPUT SYMBOL)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "embed_file.cmake needs -D${var}=...")
    endif()
endforeach()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR byte_count "${hex_length} / 2")
# Twenty bytes per line keeps the generated file readable in an editor (CMake's regex has no {n}).
string(REPEAT "[0-9a-f][0-9a-f]" 20 line_pattern)
string(REGEX REPLACE "(${line_pattern})" "\\1\n" hex "${hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
get_filename_component(input_name "${INPUT}" NAME)
file(WRITE "${OUTPUT}.tmp"
    "// Generated from ${input_name} by src/launcher/embed_file.cmake; do not edit.\n"
    "#include <cstddef>\n"
    "namespace dq8::launcher {\n"
    "extern const unsigned char ${SYMBOL}[];\n"
    "extern const std::size_t ${SYMBOL}_size;\n"
    "const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
    "const std::size_t ${SYMBOL}_size = ${byte_count};\n"
    "} // namespace dq8::launcher\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
