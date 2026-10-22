# dq8-movie: notice

`dq8-movie` (`libdq8-movie.so` on Linux) is the movie decoder of DQ8-Native (ADR-0015). It is one shared library
made of:

| Part | License | Source |
|---|---|---|
| Our wrapper, `src/runtime/movie/decoder/dq8_movie.cpp` (adapts PS2Recomp's decoder set-up, attributed) | GPL-3.0-only | this repository |
| [FFmpeg](https://ffmpeg.org) libavcodec and libavutil (the `mpeg2video` decoder), dynamically linked | LGPL-2.1-or-later (the system's build) | the system's packages |

FFmpeg is not copied into this library: the system's `libavcodec` and `libavutil` are linked dynamically, so they
can be replaced like any shared library. A bundled LGPL build of FFmpeg comes with packaging (M5).

## Using another decoder build

The rest of DQ8-Native never links FFmpeg: `dq8-game` loads this library at run time and calls only the eight
`dq8_mvd_*` functions of `include/dq8/movie/movie_decoder_abi.h` (C ABI version 1). To use your own build:

1. Get the DQ8-Native source of your release.
2. Build only this library against the FFmpeg you want (`pkg-config` must find its `libavcodec` and `libavutil`):
   `cmake --preset release -DDQ8_FFMPEG=system && cmake --build --preset release --target dq8_movie`.
3. Replace the installed `dq8-movie` file with the one built next to `dq8-game`, pass `--movie-decoder <file>` to
   `dq8-game`, or set `DQ8_MOVIE_LIBRARY` to its path.

`dq8-game` checks the library's `dq8_mvd_abi_version()` and refuses a library of another ABI version. Without the
library, `--movies auto` (the default) skips the movies and `--movies play` exits with status 3.

## Diagnostics

| Variable | Effect |
|---|---|
| `DQ8_MVD_VERBOSE=1` | Prints FFmpeg's verbose messages (errors are always printed) |
