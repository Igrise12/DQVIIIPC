#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Turn contributor function maps into published maps (ADR-0017, Docs/formats/function-map.md).

Reads the Ghidra maps tools/ghidra/export_maps.sh writes (three files per unit in work/recomp/maps) and writes
data/maps/<profile>/<unit>.toml, keeping only what the published format allows: each function's start and
size, the link-return entries, each jump table's address, base register and entry count, and the names the
runtime's call list knows (include/dq8/abi/ps2recomp/ps2_call_list.h). Every other name, Ghidra's placeholders
and thunk names included, is dropped; the setup generates names from the address. The jump tables' entries are
dropped too: the setup reads them from the user's disc.

Refuses (exit 1) a table whose indices are not 0..n-1, a function outside its unit, overlapping functions and
anything else the published format would not hold. Run with ``python3 -I``; it reads only the maps, the region
profile and the call list, never the disc.

  python3 -I tools/ghidra/publish_maps.py                  # work/recomp/maps -> data/maps/SLUS_21207/
  python3 -I tools/ghidra/publish_maps.py --check          # exit 1 when data/maps differs from what it would write
"""
from __future__ import annotations

import argparse
import csv
import io
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FORMAT = 1
CALL_LIST = ROOT / "include" / "dq8" / "abi" / "ps2recomp" / "ps2_call_list.h"


class PublishError(Exception):
    pass


def call_list_names(path: Path) -> tuple[list[str], list[str]]:
    """The kernel-call and library-stub names of the call list, in its order."""
    text = path.read_text(encoding="utf-8")
    out = []
    for macro in ("PS2_SYSCALL_LIST", "PS2_STUB_LIST"):
        m = re.search(r"#define\s+" + macro + r"\(X\)((?:.*\\\n)*.*)", text)
        if not m:
            raise PublishError(f"{path}: no {macro}")
        out.append(re.findall(r"\bX\((\w+)\)", m.group(1)))
    return out[0], out[1]


def resolve(name: str, entries: list[str]) -> str | None:
    """The recompiler's lookup: an exact match, else one leading underscore removed or added."""
    if name in entries:
        return name
    if name.startswith("_"):
        return name[1:] if name[1:] in entries else None
    return "_" + name if "_" + name in entries else None


def number(value: object, where: str) -> int:
    if isinstance(value, bool):
        raise PublishError(f"{where}: {value!r} is not a number")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            pass
    raise PublishError(f"{where}: {value!r} is not a number")


def read_profile(regions: Path, profile_id: str) -> dict:
    path = regions / f"{profile_id}.toml"
    with path.open("rb") as fh:
        profile = tomllib.load(fh)
    if profile.get("profile", {}).get("id") != profile_id:
        raise PublishError(f"{path}: [profile] id is not {profile_id}")
    return profile


def unit_sources(profile: dict) -> dict[str, str]:
    """unit name -> the SHA-1 the profile records for the unit's file, in pipeline order."""
    units = {"main": profile["boot"]["sha1"].lower()}
    order = profile.get("overlay_window", {}).get("name_table_order", [])
    rows = {row["name"]: row for row in profile.get("overlays", [])}
    for name in [*order, *[n for n in rows if n not in order]]:
        sha1 = rows[name].get("sha1", "").lower()
        if not re.fullmatch(r"[0-9a-f]{40}", sha1):
            raise PublishError(f"region profile: overlay {name} has no SHA-1")
        units[name.lower()] = sha1
    return units


def read_unit(maps: Path, unit: str, names: tuple[list[str], list[str]]) -> dict:
    csv_path = maps / f"{unit}.csv"
    stats_path = maps / f"{unit}.stats.toml"
    tables_path = maps / f"{unit}.jump_tables.toml"
    if not csv_path.is_file():
        raise PublishError(f"{csv_path}: missing")
    if not stats_path.is_file():
        raise PublishError(f"{stats_path}: missing (it gives the unit's code range)")
    with stats_path.open("rb") as fh:
        stats = tomllib.load(fh)
    code = (number(stats.get("unit_start"), f"{stats_path}: unit_start"),
            number(stats.get("unit_end"), f"{stats_path}: unit_end"))
    if not (code[0] < code[1] and code[0] % 4 == 0 and code[1] % 4 == 0):
        raise PublishError(f"{stats_path}: unit range {code[0]:#x}..{code[1]:#x} is empty or misaligned")

    rows = list(csv.reader(io.StringIO(csv_path.read_text(encoding="utf-8"))))
    if not rows or [c.strip().lower() for c in rows[0]] != ["name", "start", "end", "size"]:
        raise PublishError(f"{csv_path}:1: the header must be Name,Start,End,Size")
    functions: list[tuple[int, int, str]] = []
    for line, row in enumerate(rows[1:], start=2):
        if not row or not "".join(row).strip():
            continue
        if len(row) != 4:
            raise PublishError(f"{csv_path}:{line}: expected 4 fields")
        start = number(row[1].strip(), f"{csv_path}:{line}")
        end = number(row[2].strip(), f"{csv_path}:{line}")
        if number(row[3].strip(), f"{csv_path}:{line}") != end - start:
            raise PublishError(f"{csv_path}:{line}: Size is not End - Start")
        functions.append((start, end - start, row[0].strip()))
    functions.sort()
    for i, (start, size, name) in enumerate(functions):
        if size <= 0 or start % 4 or size % 4:
            raise PublishError(f"{csv_path}: {name} at {start:#010x} is empty or misaligned")
        if start < code[0] or start + size > code[1]:
            raise PublishError(f"{csv_path}: {name} at {start:#010x} lies outside the unit")
        if i and functions[i - 1][0] + functions[i - 1][1] > start:
            raise PublishError(f"{csv_path}: {name} at {start:#010x} overlaps {functions[i - 1][2]}")

    syscalls, stubs = names
    named = [(start, name) for start, _, name in functions if resolve(name, syscalls) or resolve(name, stubs)]
    placeholder = re.compile(r"(FUN|sub|thunk_FUN)_[0-9A-Fa-f]{8}")
    dropped = sorted({name for _, _, name in functions
                      if not placeholder.fullmatch(name) and not (resolve(name, syscalls) or resolve(name, stubs))})

    link_returns = sorted({number(a, f"{stats_path}: link_return_entries")
                           for a in stats.get("link_return_entries", [])})
    for a in link_returns:
        if a % 4 or not code[0] <= a < code[1]:
            raise PublishError(f"{stats_path}: link-return entry {a:#010x} is misaligned or outside the unit")

    tables = []
    if tables_path.is_file():
        with tables_path.open("rb") as fh:
            fragment = tomllib.load(fh)
        for t in fragment.get("jump_tables", {}).get("table", []):
            address = number(t.get("address"), f"{tables_path}: address")
            where = f"{tables_path}: the jump table at {address:#010x}"
            base = number(t.get("base_register", 0), f"{where}: base_register")
            entries = t.get("entries", [])
            indices = [number(e.get("index", i), f"{where}: index") for i, e in enumerate(entries)]
            if indices != list(range(len(indices))):
                raise PublishError(f"{where}: indices are not 0..{len(indices) - 1}; a table with gaps "
                                   "cannot be published until the gap is understood")
            if not entries or address == 0 or address % 4 or not 0 <= base <= 31:
                raise PublishError(f"{where}: empty, misaligned or with a bad base register")
            tables.append((address, base, len(entries)))
        tables.sort()
        for i in range(1, len(tables)):
            if tables[i][0] == tables[i - 1][0]:
                raise PublishError(f"{tables_path}: two tables at {tables[i][0]:#010x}")
    return {"code": code, "functions": functions, "named": named, "dropped": dropped,
            "link_returns": link_returns, "tables": tables}


def render(profile_id: str, unit: str, sha1: str, data: dict) -> str:
    out = [f"format = {FORMAT}", f'profile = "{profile_id}"', f'unit = "{unit}"', f'source_sha1 = "{sha1}"',
           f"code = [0x{data['code'][0]:08X}, 0x{data['code'][1]:08X}]", "", "functions = ["]
    out += [f"    [0x{start:08X}, 0x{size:X}]," for start, size, _ in data["functions"]]
    out.append("]")
    out.append("")
    if data["link_returns"]:
        out.append("link_return_entries = [")
        out += [f"    0x{a:08X}," for a in data["link_returns"]]
        out.append("]")
    else:
        out.append("link_return_entries = []")
    for start, name in data["named"]:
        out += ["", "[[named]]", f"start = 0x{start:08X}", f'name = "{name}"']
    for address, base, count in data["tables"]:
        out += ["", "[[jump_table]]", f"address = 0x{address:08X}", f"base_register = {base}",
                f"count = {count}"]
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--maps", type=Path, default=ROOT / "work" / "recomp" / "maps",
                    help="contributor maps (default work/recomp/maps)")
    ap.add_argument("--regions", type=Path, default=ROOT / "data" / "regions")
    ap.add_argument("--profile", default="SLUS_21207")
    ap.add_argument("--out", type=Path, default=ROOT / "data" / "maps", help="published maps root")
    ap.add_argument("--call-list", type=Path, default=CALL_LIST)
    ap.add_argument("--units", default="", help="comma-separated units (default: every unit of the profile)")
    ap.add_argument("--check", action="store_true", help="write nothing; exit 1 when a file would change")
    args = ap.parse_args(argv)

    try:
        names = call_list_names(args.call_list)
        sources = unit_sources(read_profile(args.regions, args.profile))
        units = [u for u in args.units.split(",") if u] or list(sources)
        unknown = [u for u in units if u not in sources]
        if unknown:
            raise PublishError(f"unknown units {unknown}; {args.profile} has {list(sources)}")
        outdir = args.out / args.profile
        stale = []
        for unit in units:
            data = read_unit(args.maps, unit, names)
            text = render(args.profile, unit, sources[unit], data)
            target = outdir / f"{unit}.toml"
            old = target.read_text(encoding="utf-8") if target.is_file() else None
            if args.check:
                if old != text:
                    stale.append(str(target))
            elif old != text:
                outdir.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8", newline="\n")
            print(f"{unit}: {len(data['functions'])} functions, {len(data['named'])} named, "
                  f"{len(data['link_returns'])} link-return entries, {len(data['tables'])} jump tables"
                  f" ({sum(t[2] for t in data['tables'])} entries left to the disc)"
                  + (f"; names not published: {', '.join(data['dropped'])}" if data["dropped"] else ""))
        if stale:
            print("out of date: " + ", ".join(stale), file=sys.stderr)
            return 1
    except (PublishError, OSError, KeyError, tomllib.TOMLDecodeError) as e:
        print(f"publish_maps: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
