#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# SSIM of one of our frames against a reference frame, the S5b method (Docs/spikes/S5-parallel-gs-standalone.md):
# both scaled bilinearly to 640x480, then ffmpeg's ssim filter. Prints the "All" value; with a threshold, exits 1
# when the value is below it. Frames are local files (dq8-game --dump-frame, PCSX2 screenshots) and never go in
# the repository.
#
#   tools/bringup/ssim.sh <ours.png> <reference.png> [--crop W:H:X:Y] [--min 0.98]
#
# --crop is applied after the scaling, in 640x480 coordinates (M3b acceptance 8: a logo or text crop).
set -euo pipefail

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}
[[ $# -ge 2 ]] || usage
ours=$1
reference=$2
shift 2
crop=""
min=""
while [[ $# -gt 0 ]]; do
    case $1 in
    --crop) crop=$2; shift 2 ;;
    --min) min=$2; shift 2 ;;
    *) usage ;;
    esac
done
command -v ffmpeg >/dev/null || { echo "ssim.sh: ffmpeg is not installed" >&2; exit 2; }
for f in "$ours" "$reference"; do
    [[ -f $f ]] || { echo "ssim.sh: no such file: $f" >&2; exit 2; }
done

chain="scale=640:480:flags=bilinear,format=rgb24"
if [[ -n $crop ]]; then
    chain="$chain,crop=$crop"
fi
out=$(ffmpeg -hide_banner -nostats -loglevel info -i "$ours" -i "$reference" \
    -filter_complex "[0:v]${chain}[a];[1:v]${chain}[b];[a][b]ssim" -f null - 2>&1)
value=$(grep -o 'All:[0-9.]*' <<<"$out" | tail -1 | cut -d: -f2)
if [[ -z $value ]]; then
    echo "ssim.sh: ffmpeg reported no SSIM:" >&2
    echo "$out" >&2
    exit 2
fi
echo "$value"
if [[ -n $min ]]; then
    awk -v v="$value" -v m="$min" 'BEGIN { exit (v + 0 >= m + 0) ? 0 : 1 }' || {
        echo "ssim.sh: $value is below $min" >&2
        exit 1
    }
fi
