#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Pretty-prints the SIF and RPC traffic of a dq8-game run from its JSON-lines events (--events FILE).

One line per event: vblank, cycles, the event type and its fields, with RPC server ids shown with the names the
M3 milestone document gives them. The --summary option prints a table instead: binds and calls per server, plus
the first and last vblank each server was seen. The events are our own output and hold no game data.

    python3 -I tools/bringup/rpc_trace.py run.events.jsonl [--sid 0x80000001] [--all] [--summary]

By default only SIF, RPC and IOP events are shown (types starting with sif, rpc or iop). --all shows every
event, and --sid keeps only the events for that server.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import OrderedDict

# Server ids and what they are, from Docs/milestones/M3-boots.md ("Servers bound"). Ids only; no game data.
KNOWN_SERVERS = {
    0x80000001: "fileio",
    0x80000003: "iopheap",
    0x80000006: "loadfile",
    0x80000592: "cdvd init",
    0x80000593: "cdvd S-command",
    0x80000595: "cdvd N-command",
    0x80000597: "cdvd SearchFile",
    0x8000059A: "cdvd disc ready",
    0x8000059C: "unknown (0x8000059C)",
    0x80000100: "padman",
    0x80000101: "libpad (old)",
    0x80000400: "mcserv",
    0x80000701: "sdrdrv",
    0x00123456: "PCMPLAY",
}

FIXED = ("ev", "vblank", "cycles")


def parse_number(value) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return None
    return None


def server_label(sid) -> str:
    n = parse_number(sid)
    if n is None:
        return str(sid)
    name = KNOWN_SERVERS.get(n)
    return f"0x{n:08X}" + (f" ({name})" if name else "")


def format_value(key: str, value) -> str:
    if key in ("sid", "server_id"):
        return server_label(value)
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int) and key in ("addr", "address", "buffer", "dest", "src", "pc"):
        return f"0x{value:08X}"
    return str(value)


def wanted(event: dict, show_all: bool, sid: int | None) -> bool:
    if sid is not None and parse_number(event.get("sid")) != sid:
        return False
    if show_all:
        return True
    return str(event.get("ev", "")).startswith(("sif", "rpc", "iop"))


def read_events(path: str):
    stream = sys.stdin if path == "-" else open(path, encoding="utf-8")
    with stream:
        for number, line in enumerate(stream, 1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as e:
                print(f"{path}:{number}: not JSON ({e.msg}); skipped", file=sys.stderr)
                continue
            if isinstance(event, dict) and "ev" in event:
                yield event


def print_lines(events, show_all: bool, sid: int | None) -> int:
    shown = 0
    for event in events:
        if not wanted(event, show_all, sid):
            continue
        fields = " ".join(f"{k}={format_value(k, v)}" for k, v in event.items() if k not in FIXED)
        print(f"{event.get('vblank', '?'):>7} {event.get('cycles', '?'):>14}  {event['ev']:<16} {fields}")
        shown += 1
    return shown


def print_summary(events, sid: int | None) -> int:
    servers: OrderedDict[int | str, dict] = OrderedDict()
    for event in events:
        if "sid" not in event or (sid is not None and parse_number(event["sid"]) != sid):
            continue
        key = parse_number(event["sid"])
        key = key if key is not None else str(event["sid"])
        row = servers.setdefault(key, {"binds": 0, "calls": 0, "other": 0, "first": None, "last": None})
        kind = str(event["ev"])
        if kind == "rpc_bind":
            row["binds"] += 1
        elif kind == "rpc_call":
            row["calls"] += 1
        else:
            row["other"] += 1
        vblank = event.get("vblank")
        row["first"] = vblank if row["first"] is None else row["first"]
        row["last"] = vblank
    print(f"{'server':<34} {'binds':>6} {'calls':>8} {'other':>6} {'first':>8} {'last':>8}")
    for key, row in servers.items():
        print(f"{server_label(key):<34} {row['binds']:>6} {row['calls']:>8} {row['other']:>6} "
              f"{row['first']!s:>8} {row['last']!s:>8}")
    return len(servers)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("events", help="dq8-game --events output (JSON lines), or - for stdin")
    parser.add_argument("--sid", help="only this RPC server id (decimal or 0x hex)")
    parser.add_argument("--all", action="store_true", help="every event, not only SIF, RPC and IOP ones")
    parser.add_argument("--summary", action="store_true", help="a per-server table instead of the event lines")
    args = parser.parse_args(argv)
    sid = None
    if args.sid is not None:
        sid = parse_number(args.sid)
        if sid is None:
            parser.error(f"--sid {args.sid}: not a number")
    events = read_events(args.events)
    try:
        count = print_summary(events, sid) if args.summary else print_lines(events, args.all, sid)
    except OSError as e:
        print(f"rpc_trace: {e}", file=sys.stderr)
        return 2
    if count == 0:
        print("rpc_trace: no matching events", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
