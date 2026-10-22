#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Tests for the published-map rules of scripts/check_no_game_data.py and for tools/ghidra/publish_maps.py.

Synthetic maps only: made-up addresses in [0x00200000, 0x00201000), our own names and names from the runtime's
call list. Run with ``python3 -I tests/unit/test_published_map_tools.py`` (ctest: scripts.published_map_tools).
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.dont_write_bytecode = True  # no __pycache__ next to the scripts under test


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


checker = load("check_no_game_data", ROOT / "scripts" / "check_no_game_data.py")
publisher = load("publish_maps", ROOT / "tools" / "ghidra" / "publish_maps.py")

SHA1 = "0123456789abcdef0123456789abcdef01234567"
MAP = f"""format = 1
profile = "TEST_00000"
unit = "main"
source_sha1 = "{SHA1}"
code = [0x00200000, 0x00201000]

functions = [
    [0x00200000, 0x40],
    [0x00200040, 0x40],
    [0x00200100, 0x10],
]

link_return_entries = [0x00200048]

[[named]]
start = 0x00200100
name = "_FlushCache"

[[jump_table]]
address = 0x00200800
base_register = 2
count = 3
"""
REL = "data/maps/TEST_00000/main.toml"


def problems(text: str, rel: str = REL) -> list[str]:
    return checker.published_map_problems(rel, text.encode(), checker.call_list_names())


def check(rel: str, data: bytes, release: bool = False) -> list[str]:
    c = checker.Checker(deep=False, release=release)
    c.check(rel, data)
    return c.findings


class PublishedMapRules(unittest.TestCase):
    def test_the_format_passes(self) -> None:
        self.assertEqual(problems(MAP), [])
        self.assertEqual(check(REL, MAP.encode()), [])
        self.assertEqual(check("usr/share/dq8/maps/TEST_00000/main.toml", MAP.encode(), release=True), [])

    def test_refusals(self) -> None:
        cases = {
            "entries": MAP + "entries = [0x00200040]\n",
            "comment": "# a note\n" + MAP,
            "unknown key": MAP.replace('unit = "main"', 'unit = "main"\nnote = "text"'),
            "invented name": MAP.replace("_FlushCache", "UpdateHero"),
            "placeholder name": MAP.replace("_FlushCache", "FUN_00200100"),
            "name at no start": MAP.replace("start = 0x00200100", "start = 0x00200104"),
            "overlap": MAP.replace("[0x00200040, 0x40]", "[0x00200020, 0x40]"),
            "outside": MAP.replace("[0x00200100, 0x10]", "[0x00200FF8, 0x10]"),
            "string address": MAP.replace("[0x00200000, 0x40]", '["0x00200000", 0x40]'),
            "upper-case sha1": MAP.replace(SHA1, SHA1.upper()),
            "wrong unit": MAP.replace('unit = "main"', 'unit = "menu"'),
            "format 2": MAP.replace("format = 1", "format = 2"),
            "register 32": MAP.replace("base_register = 2", "base_register = 32"),
            "count 0": MAP.replace("count = 3", "count = 0"),
            "table key": MAP.replace("count = 3", "count = 3\nnote = 1"),
            "not toml": MAP + "[[",
        }
        for label, text in cases.items():
            with self.subTest(label):
                self.assertNotEqual(problems(text), [], label)
        findings = check(REL, (MAP + "entries = [0x00200040]\n").encode())
        self.assertTrue(any("published-map" in f for f in findings), findings)

    def test_paths(self) -> None:
        self.assertNotEqual(problems(MAP, "data/maps/OTHER_00000/main.toml"), [])
        self.assertNotEqual(problems(MAP, "data/maps/TEST_00000/menu.toml"), [])


class ReleaseTrees(unittest.TestCase):
    @staticmethod
    def elf(machine: int) -> bytes:
        header = bytearray(b"\x7fELF\x01\x01\x01" + bytes(9) + bytes(36))
        header[18:20] = machine.to_bytes(2, "little")
        return bytes(header) + bytes(2 * 1024 * 1024)  # large and binary

    def test_host_binaries_pass_only_in_release_mode(self) -> None:
        host = self.elf(62)  # x86-64
        self.assertEqual(check("usr/bin/dq8-game", host, release=True), [])
        self.assertNotEqual(check("usr/bin/dq8-game", host), [])

    def test_mips_code_is_refused_in_release_mode(self) -> None:
        findings = check("usr/lib/unit.so", self.elf(8), release=True)
        self.assertTrue(any("binary-file" in f for f in findings), findings)

    def test_region_profiles_keep_their_serial_names(self) -> None:
        text = b'[profile]\nid = "TEST_00000"\n'
        self.assertEqual(check("usr/share/dq8/regions/SLUS_00000.toml", text, release=True), [])
        self.assertNotEqual(check("usr/share/dq8/other/SLUS_00000.toml", text, release=True), [])


class PublishMaps(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "regions").mkdir()
        (self.dir / "regions" / "TEST_00000.toml").write_text(
            f'[profile]\nid = "TEST_00000"\n[boot]\nsha1 = "{SHA1}"\n[overlay_window]\nname_table_order = []\n')
        maps = self.dir / "maps"
        maps.mkdir()
        (maps / "main.csv").write_text(
            "Name,Start,End,Size\n"
            "FUN_00200000,0x00200000,0x00200040,64\n"
            "thunk_FUN_00200100,0x00200040,0x00200048,8\n"
            "UpdateHero,0x00200048,0x00200080,56\n"
            "_FlushCache,0x00200100,0x00200110,16\n")
        (maps / "main.stats.toml").write_text(
            "unit_start = 0x00200000\nunit_end = 0x00201000\nlink_return_entries = [0x00200050]\n"
            "pointer_gap_functions = [0x00200040]\n")
        self.write_tables("0, 1, 2")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_tables(self, indices: str) -> None:
        entries = ", ".join(f'{{ index = {i}, target = "0x00200040" }}' for i in indices.split(", "))
        (self.dir / "maps" / "main.jump_tables.toml").write_text(
            f'[[jump_tables.table]]\naddress = "0x00200800"\nbase_register = 2\nentries = [{entries}]\n')

    def run_tool(self, *extra: str) -> int:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            return publisher.main(["--maps", str(self.dir / "maps"), "--regions", str(self.dir / "regions"),
                                   "--profile", "TEST_00000", "--out", str(self.dir / "out"), *extra])

    def test_publishes_shapes_and_call_list_names_only(self) -> None:
        self.assertEqual(self.run_tool(), 0)
        text = (self.dir / "out" / "TEST_00000" / "main.toml").read_text()
        self.assertEqual(problems(text), [])
        self.assertIn("[0x00200048, 0x38]", text)
        self.assertIn('name = "_FlushCache"', text)
        for gone in ("UpdateHero", "thunk", "FUN_", "\nentries", "index", "target", "{", "pointer_gap"):
            self.assertNotIn(gone, text)
        self.assertIn("count = 3", text)
        self.assertIn("0x00200050", text)
        self.assertEqual(self.run_tool("--check"), 0)
        (self.dir / "out" / "TEST_00000" / "main.toml").write_text(text.replace("count = 3", "count = 4"))
        self.assertEqual(self.run_tool("--check"), 1)

    def test_refuses_tables_with_index_gaps(self) -> None:
        self.write_tables("0, 2, 3")
        self.assertEqual(self.run_tool(), 1)
        self.assertFalse((self.dir / "out" / "TEST_00000" / "main.toml").exists())


if __name__ == "__main__":
    unittest.main()
