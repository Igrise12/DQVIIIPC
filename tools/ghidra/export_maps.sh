#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#
# Export the function maps of the seven code units (the main program and the six overlays) for
# ps2_recomp: one read-only headless run of ExportFunctionMap.java per unit.
#
#   tools/ghidra/export_maps.sh [--units LIST] [--out DIR] [--projects DIR] [--elf FILE] [--bin-dir DIR]
#                               [--dq8ctl FILE]
#
#   --units     comma-separated subset of: main,title,casino,viewer,battle,menu,shop (default: all)
#   --out       output directory (default: work/recomp/maps); per unit <unit>.csv,
#               <unit>.jump_tables.toml, <unit>.stats.toml, and logs/export-<unit>.log
#   --projects  Ghidra projects (default: work/ghidra): base/ for main (spike S2, with the bss and
#               overlay_window blocks) and s3-<unit>/ per overlay (spike S3, LoadOverlay.java)
#   --elf       boot ELF (default: baserom/raw/SLUS_212.07); its file name is the program name in
#               every project
#   --bin-dir   directory with the overlay containers <UNIT>.BIN (default: next to --elf, BIN/)
#   --dq8ctl    dq8ctl binary (default: build/dev-clang/src/cli/dq8ctl)
#
# Unit ranges come from `dq8ctl info --json`: main is the boot ELF's first PT_LOAD with file bytes
# ([vaddr, vaddr + filesz)); an overlay is [load_address, load_address + 64 + text_size + data_size).
# The projects are opened with -readOnly and never saved. Ghidra runs under nice (DQ8_NICE, default
# 19) with -max-cpu (DQ8_GHIDRA_MAX_CPU, default 4). The outputs are game-derived: they stay under
# work/ and are never committed. Contributor tool only; never on the end-user setup path.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"

all_units="main,title,casino,viewer,battle,menu,shop"
units=$all_units
out=work/recomp/maps
projects=work/ghidra
elf=baserom/raw/SLUS_212.07
bin_dir=
dq8ctl=build/dev-clang/src/cli/dq8ctl

usage() {
    sed -n '6,20p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --units) units=${2:?}; shift 2 ;;
        --out) out=${2:?}; shift 2 ;;
        --projects) projects=${2:?}; shift 2 ;;
        --elf) elf=${2:?}; shift 2 ;;
        --bin-dir) bin_dir=${2:?}; shift 2 ;;
        --dq8ctl) dq8ctl=${2:?}; shift 2 ;;
        -h|--help) usage ;;
        *) echo "error: unknown argument: $1" >&2; usage ;;
    esac
done
bin_dir=${bin_dir:-$(dirname "$elf")/BIN}
program=$(basename "$elf")

for tool in "$dq8ctl" "$elf"; do
    if [ ! -e "$tool" ]; then
        echo "error: $tool not found" >&2
        exit 1
    fi
done

# Prints "<start> <end>" for a unit, from dq8ctl's JSON description of the ELF or the container.
unit_range() {
    local file=$1
    "$dq8ctl" info "$file" --json | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
if not d.get("ok"):
    sys.exit("dq8ctl info failed: %s" % d.get("error"))
if d["kind"] == "elf":
    seg = next(s for s in d["segments"] if s["type"] == 1 and int(s["filesz"], 16) > 0)
    start = int(seg["vaddr"], 16)
    end = start + int(seg["filesz"], 16)
elif d["kind"] == "mwo3":
    h = d["header"]
    start = int(h["load_address"], 16)
    end = start + 0x40 + int(h["text_size"], 16) + int(h["data_size"], 16)
else:
    sys.exit("unexpected kind " + d["kind"])
print("0x%08X 0x%08X" % (start, end))
'
}

mkdir -p "$out/logs"
IFS=, read -r -a selected <<< "$units"
for unit in "${selected[@]}"; do
    case ",$all_units," in
        *",$unit,"*) ;;
        *) echo "error: unknown unit '$unit' (expected one of $all_units)" >&2; exit 2 ;;
    esac
    # Code that only data pointers reach (vtables, callbacks) becomes functions too (M3 bring-up).
    extra=(pointer_gaps=1)
    if [ "$unit" = main ]; then
        project=$projects/base
        read -r start end < <(unit_range "$elf")
        # The static-constructor table from the region profile whose boot path is this ELF ([boot].ctor_table).
        tables=$(python3 -I - "$repo/data/regions" "$program" <<'PY'
import pathlib, sys, tomllib
for f in sorted(pathlib.Path(sys.argv[1]).glob("*.toml")):
    boot = tomllib.loads(f.read_text()).get("boot", {})
    if pathlib.PurePosixPath(boot.get("path", "")).name == sys.argv[2] and "ctor_table" in boot:
        s, e = boot["ctor_table"]
        print("entry_tables=0x%08X:0x%08X" % (s, e))
        break
PY
)
        if [ -n "$tables" ]; then
            extra+=("$tables")
        fi
        # Main-program targets of the overlays' direct jal/j instructions: functions only overlays call.
        targets=$(python3 -I - "$repo/data/regions" "$program" "$bin_dir" <<'PY'
import pathlib, struct, sys, tomllib
for f in sorted(pathlib.Path(sys.argv[1]).glob("*.toml")):
    prof = tomllib.loads(f.read_text())
    boot = prof.get("boot", {})
    if pathlib.PurePosixPath(boot.get("path", "")).name != sys.argv[2]:
        continue
    lo, hi = boot["load_address"], boot["load_address"] + boot["main_memsz"]
    window = prof["overlay_window"]
    found = set()
    for o in prof.get("overlays", []):
        data = (pathlib.Path(sys.argv[3]) / pathlib.PurePosixPath(o["path"]).name).read_bytes()
        start, end = window["header_size"], window["header_size"] + o["text_size"] + o["data_size"]
        for off in range(start, min(end, len(data)) - 3, 4):
            w = struct.unpack_from("<I", data, off)[0]
            if (w >> 26) in (2, 3):  # j, jal
                pc = window["base"] + off
                target = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if lo <= target < hi:
                    found.add(target)
    if found:
        print("entries=" + ",".join("0x%08X" % t for t in sorted(found)))
    break
PY
)
        if [ -n "$targets" ]; then
            extra+=("$targets")
        fi
    else
        project=$projects/s3-$unit
        read -r start end < <(unit_range "$bin_dir/${unit^^}.BIN")
        # Overlay code reached only through the main program's callback tables (M4 bring-up B2).
        extra+=(outside_pointers=1)
    fi
    if [ ! -f "$project/DQ8.gpr" ]; then
        echo "error: Ghidra project $project/DQ8.gpr not found" >&2
        exit 1
    fi
    log=$out/logs/export-$unit.log
    t0=$(date +%s.%N)
    if ! nice -n "${DQ8_NICE:-19}" "$repo/tools/ghidra/headless.sh" "$project" DQ8 -process "$program" \
        -noanalysis -readOnly -max-cpu "${DQ8_GHIDRA_MAX_CPU:-4}" \
        -postScript ExportFunctionMap.java "$out/$unit.csv" "$out/$unit.jump_tables.toml" \
        "$out/$unit.stats.toml" "$start" "$end" "${extra[@]}" > "$log" 2>&1; then
        echo "error: headless run for $unit failed; see $log" >&2
        exit 1
    fi
    t1=$(date +%s.%N)
    summary=$(grep -o 'ExportFunctionMap: unit .*;' "$log" || true)
    if [ -z "$summary" ] || grep -q 'SCRIPT ERROR' "$log"; then
        echo "error: ExportFunctionMap did not complete for $unit; see $log" >&2
        exit 1
    fi
    printf '%-7s %s-%s  %5.1f s  %s\n' "$unit" "$start" "$end" "$(awk "BEGIN { print $t1 - $t0 }")" \
        "${summary#ExportFunctionMap: unit * }"
done
