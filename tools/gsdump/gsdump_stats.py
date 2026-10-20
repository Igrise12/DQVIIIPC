#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 DQ8-Native contributors
"""Feature histogram of raw PCSX2 GS dumps (.gs, GSDumpCompression=0) for spike S7.

Contributor tool, Python 3 standard library only. Reads the dump container (2.x header, state blob, privileged
registers, transfer / vsync / readback / register packets), walks every GIF tag on PATH1-3 and reports what the
game asks of the GS: register-write histogram (A+D included), PSM values of FRAME, ZBUF, TEX0 and BITBLTBUF,
primitive types and vertex kicks, alpha-blend formulas, TEST modes, mipmapping and CLUT use, dithering, PABE,
FBA, local-to-host readbacks, image-transfer bytes, draws per frame and the CRTC (PMODE, SMODE2, DISPFB,
DISPLAY) state at every vsync. Output is counts and register fields only; no dump bytes are echoed.

  python3 -I tools/gsdump/gsdump_stats.py DUMP.gs [DUMP2.gs ...] [--json OUT.json] [--markdown | --table]
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

REG_NAMES = {
    0x00: "PRIM", 0x01: "RGBAQ", 0x02: "ST", 0x03: "UV", 0x04: "XYZF2", 0x05: "XYZ2", 0x06: "TEX0_1",
    0x07: "TEX0_2", 0x08: "CLAMP_1", 0x09: "CLAMP_2", 0x0A: "FOG", 0x0C: "XYZF3", 0x0D: "XYZ3",
    0x0E: "A+D", 0x0F: "NOP", 0x14: "TEX1_1", 0x15: "TEX1_2", 0x16: "TEX2_1", 0x17: "TEX2_2",
    0x18: "XYOFFSET_1", 0x19: "XYOFFSET_2", 0x1A: "PRMODECONT", 0x1B: "PRMODE", 0x1C: "TEXCLUT",
    0x22: "SCANMSK", 0x34: "MIPTBP1_1", 0x35: "MIPTBP1_2", 0x36: "MIPTBP2_1", 0x37: "MIPTBP2_2",
    0x3B: "TEXA", 0x3D: "FOGCOL", 0x3F: "TEXFLUSH", 0x40: "SCISSOR_1", 0x41: "SCISSOR_2", 0x42: "ALPHA_1",
    0x43: "ALPHA_2", 0x44: "DIMX", 0x45: "DTHE", 0x46: "COLCLAMP", 0x47: "TEST_1", 0x48: "TEST_2",
    0x49: "PABE", 0x4A: "FBA_1", 0x4B: "FBA_2", 0x4C: "FRAME_1", 0x4D: "FRAME_2", 0x4E: "ZBUF_1",
    0x4F: "ZBUF_2", 0x50: "BITBLTBUF", 0x51: "TRXPOS", 0x52: "TRXREG", 0x53: "TRXDIR", 0x54: "HWREG",
    0x60: "SIGNAL", 0x61: "FINISH", 0x62: "LABEL",
}
PSM_NAMES = {0: "PSMCT32", 1: "PSMCT24", 2: "PSMCT16", 10: "PSMCT16S", 19: "PSMT8", 20: "PSMT4", 27: "PSMT8H",
             36: "PSMT4HL", 44: "PSMT4HH", 48: "PSMZ32", 49: "PSMZ24", 50: "PSMZ16", 58: "PSMZ16S"}
CLUT_PSMS = {19, 20, 27, 36, 44}
PRIM_NAMES = {0: "POINT", 1: "LINE", 2: "LINESTRIP", 3: "TRIANGLE", 4: "TRISTRIP", 5: "TRIFAN", 6: "SPRITE", 7: "INVALID"}
PRIM_VERTS = {0: (1, 1), 1: (2, 2), 2: (2, 1), 3: (3, 3), 4: (3, 1), 5: (3, 1), 6: (2, 2), 7: (1, 1)}
PATH_NAMES = {0: "PATH1(legacy)", 1: "PATH2(legacy)", 2: "PATH3(legacy)", 3: "GIF(unified)", 4: "dummy"}
ATST_NAMES = {0: "NEVER", 1: "ALWAYS", 2: "LESS", 3: "LEQUAL", 4: "EQUAL", 5: "GEQUAL", 6: "GREATER", 7: "NOTEQUAL"}
ZTST_NAMES = {0: "NEVER", 1: "ALWAYS", 2: "GEQUAL", 3: "GREATER"}
AFAIL_NAMES = {0: "KEEP", 1: "FB_ONLY", 2: "ZB_ONLY", 3: "RGB_ONLY"}
TFX_NAMES = {0: "MODULATE", 1: "DECAL", 2: "HIGHLIGHT", 3: "HIGHLIGHT2"}
WM_NAMES = {0: "REPEAT", 1: "CLAMP", 2: "REGION_CLAMP", 3: "REGION_REPEAT"}
MMIN_NAMES = {0: "NEAREST", 1: "LINEAR", 2: "NEAREST_MIP_NEAREST", 3: "LINEAR_MIP_NEAREST",
              4: "NEAREST_MIP_LINEAR", 5: "LINEAR_MIP_LINEAR"}
BLEND_NAMES = ({0: "Cs", 1: "Cd", 2: "0"}, {0: "Cs", 1: "Cd", 2: "0"}, {0: "As", 1: "Ad", 2: "FIX"}, {0: "Cs", 1: "Cd", 2: "0"})
STATE_REGS = ("PRIM", "TEX0_1", "TEX0_2", "TEX1_1", "TEX1_2", "CLAMP_1", "CLAMP_2", "ALPHA_1", "ALPHA_2", "TEST_1",
              "TEST_2", "FRAME_1", "FRAME_2", "ZBUF_1", "ZBUF_2", "SCISSOR_1", "SCISSOR_2", "XYOFFSET_1",
              "XYOFFSET_2", "FBA_1", "FBA_2", "PRMODE", "COLCLAMP", "DTHE", "PABE", "TEXA", "FOGCOL")
U32 = struct.Struct("<I")
U64 = struct.Struct("<Q")
HEADER = struct.Struct("<9I")


def bits(value: int, low: int, count: int) -> int:
    return (value >> low) & ((1 << count) - 1)


def blend_formula(alpha: int) -> str:
    a, b, c, d = bits(alpha, 0, 2), bits(alpha, 2, 2), bits(alpha, 4, 2), bits(alpha, 6, 2)
    fix = bits(alpha, 32, 8)
    text = f"({BLEND_NAMES[0].get(a, '?')} - {BLEND_NAMES[1].get(b, '?')}) * {BLEND_NAMES[2].get(c, '?')} + {BLEND_NAMES[3].get(d, '?')}"
    return f"{text} [FIX={fix}]" if c == 2 else text


def test_modes(test: int) -> str:
    ate, atst, aref, afail = bits(test, 0, 1), bits(test, 1, 3), bits(test, 4, 8), bits(test, 12, 2)
    date, datm, zte, ztst = bits(test, 14, 1), bits(test, 15, 1), bits(test, 16, 1), bits(test, 17, 2)
    parts = []
    parts.append(f"ATE={ATST_NAMES[atst]}/{aref}/{AFAIL_NAMES[afail]}" if ate else "ATE=off")
    parts.append(f"DATE={datm}" if date else "DATE=off")
    parts.append(f"ZTE={ZTST_NAMES[ztst]}" if zte else "ZTE=off")
    return " ".join(parts)


class CrtcState:
    """Privileged registers (GSPrivRegSet, 8192 bytes) as they are at each vsync."""

    @staticmethod
    def parse(block: bytes) -> dict:
        def reg(offset: int) -> int:
            return U64.unpack_from(block, offset)[0]
        pmode, smode1, smode2 = reg(0x0000), reg(0x0010), reg(0x0020)
        out = {
            "PMODE": {"EN1": bits(pmode, 0, 1), "EN2": bits(pmode, 1, 1), "CRTMD": bits(pmode, 2, 3), "MMOD": bits(pmode, 5, 1),
                      "AMOD": bits(pmode, 6, 1), "SLBG": bits(pmode, 7, 1), "ALP": bits(pmode, 8, 8)},
            "SMODE1": {"CMOD": bits(smode1, 13, 2), "LC": bits(smode1, 3, 7), "T1248": bits(smode1, 10, 2), "SLCK": bits(smode1, 12, 1),
                       "EX": bits(smode1, 15, 1), "PRM": bits(smode1, 16, 3), "SINT": bits(smode1, 19, 1), "XPCK": bits(smode1, 20, 1),
                       "PCK2": bits(smode1, 21, 2), "SPML": bits(smode1, 23, 4), "GCONT": bits(smode1, 25, 1), "PHS": bits(smode1, 26, 1),
                       "PVS": bits(smode1, 27, 1), "PEHS": bits(smode1, 28, 1), "PEVS": bits(smode1, 29, 1), "CLKSEL": bits(smode1, 30, 2),
                       "NVCK": bits(smode1, 32, 1), "SLCK2": bits(smode1, 33, 1), "VCKSEL": bits(smode1, 34, 2), "VHP": bits(smode1, 36, 1),
                       "RC": bits(smode1, 0, 3)},
            "SMODE2": {"INT": bits(smode2, 0, 1), "FFMD": bits(smode2, 1, 1), "DPMS": bits(smode2, 2, 2)},
            "BGCOLOR": reg(0x00E0) & 0xFFFFFF,
            "CSR": {"FIELD": bits(reg(0x1000), 13, 1), "NFIELD": bits(reg(0x1000), 12, 1)},
        }
        for index, base in ((1, 0x0070), (2, 0x0090)):
            dispfb, display = reg(base), reg(base + 0x10)
            out[f"DISPFB{index}"] = {"FBP": bits(dispfb, 0, 9), "FBW": bits(dispfb, 9, 6), "PSM": bits(dispfb, 15, 5),
                                      "DBX": bits(dispfb, 32, 11), "DBY": bits(dispfb, 43, 11)}
            dw, dh, magh, magv = bits(display, 32, 12), bits(display, 44, 11), bits(display, 23, 4), bits(display, 27, 2)
            out[f"DISPLAY{index}"] = {"DX": bits(display, 0, 12), "DY": bits(display, 12, 11), "MAGH": magh, "MAGV": magv,
                                       "DW": dw, "DH": dh, "width": (dw + 1) // (magh + 1), "height": (dh + 1) // (magv + 1)}
        return out

    @staticmethod
    def summary(state: dict) -> str:
        pm, sm = state["PMODE"], state["SMODE2"]
        circuits = []
        for index in (1, 2):
            if pm[f"EN{index}"]:
                fb, dp = state[f"DISPFB{index}"], state[f"DISPLAY{index}"]
                circuits.append(f"RC{index}: FBP=0x{fb['FBP']:03x} FBW={fb['FBW'] * 64} PSM={PSM_NAMES.get(fb['PSM'], fb['PSM'])} "
                                f"DBX/DBY={fb['DBX']}/{fb['DBY']} DISPLAY={dp['width']}x{dp['height']} (DW={dp['DW']} DH={dp['DH']} "
                                f"MAGH={dp['MAGH']} MAGV={dp['MAGV']} DX={dp['DX']} DY={dp['DY']})")
        mode = "interlaced" if sm["INT"] else "progressive"
        if sm["INT"]:
            mode += " FRAME(FFMD=1)" if sm["FFMD"] else " FIELD(FFMD=0)"
        cmod = {2: "NTSC", 3: "PAL"}.get(state["SMODE1"]["CMOD"], f"CMOD={state['SMODE1']['CMOD']}")
        merge = f"MMOD={pm['MMOD']} ALP={pm['ALP']} SLBG={pm['SLBG']} AMOD={pm['AMOD']}"
        return f"{cmod} {mode}; {merge}; " + "; ".join(circuits or ["no circuit enabled"])


class PathState:
    __slots__ = ("nloop", "flg", "nreg", "regs", "reg_index", "image_bytes_left", "eop")

    def __init__(self) -> None:
        self.nloop = 0
        self.flg = 0
        self.nreg = 0
        self.regs: list[int] = []
        self.reg_index = 0
        self.image_bytes_left = 0
        self.eop = True


class Stats:
    def __init__(self) -> None:
        self.reg_writes: Counter = Counter()
        self.ad_writes: Counter = Counter()
        self.prim_kicks: Counter = Counter()
        self.prim_count: Counter = Counter()
        self.prim_flags: Counter = Counter()
        self.frame_psm: Counter = Counter()
        self.frame_fbw: Counter = Counter()
        self.frame_fbmsk: Counter = Counter()
        self.frame_fbp: Counter = Counter()
        self.zbuf_psm: Counter = Counter()
        self.zbuf_zmsk: Counter = Counter()
        self.zbuf_zbp: Counter = Counter()
        self.tex0_psm: Counter = Counter()
        self.tex0_cpsm: Counter = Counter()
        self.tex0_tfx: Counter = Counter()
        self.tex0_tcc: Counter = Counter()
        self.tex0_size: Counter = Counter()
        self.tex0_cld: Counter = Counter()
        self.tex0_csm: Counter = Counter()
        self.tex0_tbp: Counter = Counter()
        self.tex1: Counter = Counter()
        self.tex1_mxl: Counter = Counter()
        self.clamp: Counter = Counter()
        self.alpha: Counter = Counter()
        self.test: Counter = Counter()
        self.bitblt_dpsm: Counter = Counter()
        self.bitblt_spsm: Counter = Counter()
        self.trxdir: Counter = Counter()
        self.trxreg_sizes: Counter = Counter()
        self.readback_pixels = 0
        self.local_copy_pixels = 0
        self.image_bytes = 0
        self.image_bytes_by_psm: Counter = Counter()
        self.dthe: Counter = Counter()
        self.pabe: Counter = Counter()
        self.fba: Counter = Counter()
        self.colclamp: Counter = Counter()
        self.scanmsk: Counter = Counter()
        self.prmodecont: Counter = Counter()
        self.texa: Counter = Counter()
        self.dimx: Counter = Counter()
        self.fogcol: Counter = Counter()
        self.fog_writes = 0
        self.gif_tags = 0
        self.gif_flg: Counter = Counter()
        self.path_bytes: Counter = Counter()
        self.path_tags: Counter = Counter()
        self.transfers: Counter = Counter()
        self.readfifo_packets = 0
        self.readfifo_bytes = 0
        self.vsyncs = 0
        self.fields: list[int] = []
        self.per_vsync: list[dict] = []
        self.crtc_per_vsync: list[dict] = []
        self.draws = 0
        self.vertices = 0
        self.kicks = 0
        self.render_targets: set[int] = set()
        self.depth_targets: set[int] = set()
        self.texture_bases: set[int] = set()
        self.ctxt_used: Counter = Counter()
        self.signal_finish_label: Counter = Counter()
        # running state
        self.prim = 0
        self.prim_type = 0
        self.queue = 0
        self.prim_started = False
        self.state_dirty = True
        self.cur_frame = {1: None, 2: None}
        self.cur_zbuf = {1: None, 2: None}
        self.cur_tex0 = {1: None, 2: None}
        self.frame_counters = self._new_frame_counters()

    @staticmethod
    def _new_frame_counters() -> dict:
        return {"kicks": 0, "prims": 0, "draws": 0, "image_bytes": 0, "readbacks": 0, "gif_tags": 0,
                "frame_writes": 0, "tex0_writes": 0, "fb_as_texture_tex0": 0, "transfer_bytes": 0}

    # ---- register handlers -----------------------------------------------------------------------------------
    def write(self, reg: int, value: int, path: int) -> None:
        name = REG_NAMES.get(reg, f"0x{reg:02x}")
        self.reg_writes[name] += 1
        if reg == 0x00:
            self.prim = value & 0x7FF
            self.prim_type = value & 7
            self.queue = 0
            self.prim_started = False
            self.state_dirty = True
            self.prim_flags[self.flag_key(value)] += 1
        elif reg in (0x04, 0x05, 0x0C, 0x0D):
            self.vertex(reg, value, adc=(reg in (0x0C, 0x0D)))
        elif reg in (0x06, 0x07):
            ctx = 1 if reg == 0x06 else 2
            self.tex0(value, ctx)
        elif reg in (0x14, 0x15):
            mxl, mmin, lcm, mtba = bits(value, 2, 3), bits(value, 6, 3), bits(value, 0, 1), bits(value, 9, 1)
            self.tex1[f"MXL={mxl} MMAG={'LINEAR' if bits(value, 5, 1) else 'NEAREST'} MMIN={MMIN_NAMES.get(mmin, mmin)} "
                      f"LCM={lcm} MTBA={mtba}"] += 1
            self.tex1_mxl[mxl] += 1
            self.state_dirty = True
        elif reg in (0x08, 0x09):
            self.clamp[f"WMS={WM_NAMES[bits(value, 0, 2)]} WMT={WM_NAMES[bits(value, 2, 2)]}"] += 1
            self.state_dirty = True
        elif reg in (0x42, 0x43):
            self.alpha[blend_formula(value)] += 1
            self.state_dirty = True
        elif reg in (0x47, 0x48):
            self.test[test_modes(value)] += 1
            self.state_dirty = True
        elif reg in (0x4C, 0x4D):
            ctx = 1 if reg == 0x4C else 2
            fbp, fbw, psm, fbmsk = bits(value, 0, 9), bits(value, 16, 6), bits(value, 24, 6), bits(value, 32, 32)
            self.frame_psm[PSM_NAMES.get(psm, psm)] += 1
            self.frame_fbw[fbw * 64] += 1
            self.frame_fbmsk[f"0x{fbmsk:08x}"] += 1
            self.frame_fbp[f"0x{fbp:03x}"] += 1
            self.render_targets.add(fbp)
            self.cur_frame[ctx] = (fbp, fbw, psm)
            self.frame_counters["frame_writes"] += 1
            self.state_dirty = True
        elif reg in (0x4E, 0x4F):
            ctx = 1 if reg == 0x4E else 2
            zbp, psm, zmsk = bits(value, 0, 9), bits(value, 24, 4), bits(value, 32, 1)
            self.zbuf_psm[PSM_NAMES.get(psm | 0x30, psm)] += 1
            self.zbuf_zmsk[zmsk] += 1
            self.zbuf_zbp[f"0x{zbp:03x}"] += 1
            self.depth_targets.add(zbp)
            self.cur_zbuf[ctx] = (zbp, psm, zmsk)
            self.state_dirty = True
        elif reg == 0x50:
            spsm, dpsm = bits(value, 24, 6), bits(value, 56, 6)
            self.bitblt_spsm[PSM_NAMES.get(spsm, spsm)] += 1
            self.bitblt_dpsm[PSM_NAMES.get(dpsm, dpsm)] += 1
            self.cur_bitblt = (bits(value, 0, 14), bits(value, 16, 6), spsm, bits(value, 32, 14), bits(value, 48, 6), dpsm)
        elif reg == 0x52:
            self.cur_trxreg = (bits(value, 0, 12), bits(value, 32, 12))
        elif reg == 0x53:
            xdir = bits(value, 0, 2)
            self.trxdir[{0: "host->local", 1: "local->host", 2: "local->local", 3: "deactivated"}[xdir]] += 1
            w, h = getattr(self, "cur_trxreg", (0, 0))
            if xdir == 1:
                self.readback_pixels += w * h
                self.frame_counters["readbacks"] += 1
                self.trxreg_sizes[f"readback {w}x{h}"] += 1
            elif xdir == 2:
                self.local_copy_pixels += w * h
                self.trxreg_sizes[f"local copy {w}x{h}"] += 1
            elif xdir == 0:
                self.trxreg_sizes[f"upload {w}x{h}"] += 1
        elif reg == 0x45:
            self.dthe[value & 1] += 1
            self.state_dirty = True
        elif reg == 0x44:
            self.dimx[f"0x{value:016x}"] += 1
        elif reg == 0x49:
            self.pabe[value & 1] += 1
            self.state_dirty = True
        elif reg in (0x4A, 0x4B):
            self.fba[value & 1] += 1
            self.state_dirty = True
        elif reg == 0x46:
            self.colclamp[value & 1] += 1
            self.state_dirty = True
        elif reg == 0x22:
            self.scanmsk[value & 3] += 1
        elif reg == 0x1A:
            self.prmodecont[value & 1] += 1
        elif reg == 0x1B:
            self.prim = (self.prim & 7) | (value & 0x7F8)
            self.prim_flags["PRMODE:" + self.flag_key(value)] += 1
            self.state_dirty = True
        elif reg == 0x3B:
            self.texa[f"TA0={bits(value, 0, 8)} AEM={bits(value, 15, 1)} TA1={bits(value, 32, 8)}"] += 1
        elif reg == 0x3D:
            self.fogcol[f"0x{value & 0xFFFFFF:06x}"] += 1
        elif reg == 0x0A:
            self.fog_writes += 1
        elif reg in (0x60, 0x61, 0x62):
            self.signal_finish_label[name] += 1
        elif reg in (0x40, 0x41, 0x18, 0x19):
            self.state_dirty = True

    @staticmethod
    def flag_key(prim: int) -> str:
        flags = []
        for bit, label in ((3, "IIP"), (4, "TME"), (5, "FGE"), (6, "ABE"), (7, "AA1"), (8, "FST"), (9, "CTXT2"), (10, "FIX")):
            if bits(prim, bit, 1):
                flags.append(label)
        return PRIM_NAMES[prim & 7] + (" " + "+".join(flags) if flags else "")

    def tex0(self, value: int, ctx: int) -> None:
        tbp0, tbw, psm = bits(value, 0, 14), bits(value, 14, 6), bits(value, 20, 6)
        tw, th, tcc, tfx = bits(value, 26, 4), bits(value, 30, 4), bits(value, 34, 1), bits(value, 35, 2)
        cbp, cpsm, csm, csa, cld = bits(value, 37, 14), bits(value, 51, 4), bits(value, 55, 1), bits(value, 56, 5), bits(value, 61, 3)
        self.tex0_psm[PSM_NAMES.get(psm, psm)] += 1
        self.tex0_tfx[TFX_NAMES[tfx]] += 1
        self.tex0_tcc[tcc] += 1
        self.tex0_size[f"{1 << tw}x{1 << th}"] += 1
        if psm in CLUT_PSMS:
            self.tex0_cpsm[PSM_NAMES.get(cpsm, cpsm)] += 1
            self.tex0_csm[csm] += 1
            self.tex0_cld[cld] += 1
        self.tex0_tbp[f"0x{tbp0:04x}"] += 1
        self.texture_bases.add(tbp0)
        self.cur_tex0[ctx] = (tbp0, tbw, psm)
        self.frame_counters["tex0_writes"] += 1
        # Framebuffer-as-texture: the texture base lies inside a page range that has been a render or depth target.
        page = tbp0 >> 5  # TBP0 is in 64-word units; FBP/ZBP in 2048-word (32x) units
        if page in self.render_targets or page in self.depth_targets or psm >= 48:
            self.frame_counters["fb_as_texture_tex0"] += 1
        self.state_dirty = True

    def vertex(self, reg: int, value: int, adc: bool) -> None:
        self.vertices += 1
        self.queue += 1
        first, step = PRIM_VERTS[self.prim_type]
        completes = (self.queue == first) if not self.prim_started else ((self.queue - first) % step == 0 and self.queue > first)
        if completes:
            self.prim_started = True
            if not adc:
                self.kicks += 1
                self.prim_kicks[PRIM_NAMES[self.prim_type]] += 1
                self.prim_count[PRIM_NAMES[self.prim_type]] += 1
                self.frame_counters["kicks"] += 1
                self.frame_counters["prims"] += 1
                if self.state_dirty:
                    self.draws += 1
                    self.frame_counters["draws"] += 1
                    self.state_dirty = False
                self.ctxt_used[2 if bits(self.prim, 9, 1) else 1] += 1
            if self.prim_type in (0, 1, 3, 6):
                self.queue = 0
                self.prim_started = False

    # ---- GIF walking -----------------------------------------------------------------------------------------
    def gif(self, data: memoryview, path: int, state: PathState) -> None:
        pos, end = 0, len(data)
        self.path_bytes[PATH_NAMES.get(path, path)] += end
        while pos < end:
            if state.image_bytes_left > 0:
                take = min(state.image_bytes_left, end - pos)
                self.image_bytes += take
                self.frame_counters["image_bytes"] += take
                dpsm = getattr(self, "cur_bitblt", (0, 0, 0, 0, 0, 0))[5]
                self.image_bytes_by_psm[PSM_NAMES.get(dpsm, dpsm)] += take
                state.image_bytes_left -= take
                pos += take
                if state.image_bytes_left == 0:
                    state.nloop = 0
                continue
            if state.nloop == 0:
                if end - pos < 16:
                    break
                lo, hi = U64.unpack_from(data, pos)[0], U64.unpack_from(data, pos + 8)[0]
                pos += 16
                self.gif_tags += 1
                self.frame_counters["gif_tags"] += 1
                self.path_tags[PATH_NAMES.get(path, path)] += 1
                state.nloop = lo & 0x7FFF
                state.eop = bool(lo >> 15 & 1)
                state.flg = (lo >> 58) & 3
                state.nreg = (lo >> 60) & 15 or 16
                state.regs = [(hi >> (4 * i)) & 15 for i in range(state.nreg)]
                state.reg_index = 0
                self.gif_flg[("PACKED", "REGLIST", "IMAGE", "IMAGE(3)")[state.flg]] += 1
                if (lo >> 46) & 1 and state.flg == 0:  # PRE: PRIM comes from the tag
                    self.write(0x00, (lo >> 47) & 0x7FF, path)
                if state.nloop == 0:
                    continue
                if state.flg >= 2:
                    state.image_bytes_left = state.nloop * 16
                    continue
            if state.flg == 0:  # PACKED: one qword per register
                while state.nloop > 0 and end - pos >= 16:
                    lo, hi = U64.unpack_from(data, pos)[0], U64.unpack_from(data, pos + 8)[0]
                    pos += 16
                    self.packed(state.regs[state.reg_index], lo, hi, path)
                    state.reg_index += 1
                    if state.reg_index == state.nreg:
                        state.reg_index = 0
                        state.nloop -= 1
                if state.nloop > 0:
                    break  # continues in the next transfer packet
            elif state.flg == 1:  # REGLIST: one qword holds two registers
                while state.nloop > 0 and end - pos >= 8:
                    value = U64.unpack_from(data, pos)[0]
                    pos += 8
                    reg = state.regs[state.reg_index]
                    if reg != 0x0E and reg != 0x0F:
                        self.write(reg, value, path)
                    state.reg_index += 1
                    if state.reg_index == state.nreg:
                        state.reg_index = 0
                        state.nloop -= 1
                if state.nloop == 0 and pos % 16:
                    pos += 8  # REGLIST data is padded to a qword
                elif state.nloop > 0:
                    break

    def packed(self, reg: int, lo: int, hi: int, path: int) -> None:
        if reg == 0x0E:
            address = hi & 0xFF
            self.ad_writes[REG_NAMES.get(address, f"0x{address:02x}")] += 1
            self.write(address, lo, path)
        elif reg in (0x04, 0x05, 0x0C, 0x0D):
            adc = bool(hi >> 47 & 1) or reg in (0x0C, 0x0D)
            self.reg_writes[REG_NAMES[reg]] += 1
            self.vertex(reg, lo, adc)
        elif reg == 0x0F:
            self.reg_writes["NOP"] += 1
        elif reg == 0x00:
            self.write(0x00, lo & 0x7FF, path)
        elif reg in (0x01, 0x02, 0x03, 0x0A):
            self.reg_writes[REG_NAMES[reg]] += 1
            if reg == 0x0A:
                self.fog_writes += 1
        else:  # TEX0_1/2, CLAMP_1/2 carry the 64-bit register in the low qword
            self.write(reg, lo, path)

    def vsync(self, field: int) -> None:
        self.vsyncs += 1
        self.fields.append(field)
        self.per_vsync.append(dict(self.frame_counters))
        self.frame_counters = self._new_frame_counters()


def parse_dump(path: Path) -> dict:
    data = path.read_bytes()
    view = memoryview(data)
    crc = U32.unpack_from(data, 0)[0]
    block = U32.unpack_from(data, 4)[0]  # 2.x: size of header struct + serial + embedded screenshot; old: state size
    pos = 8
    info: dict = {"file": str(path), "size": len(data), "format": "old"}
    if crc == 0xFFFFFFFF:
        if block < HEADER.size:
            raise ValueError(f"{path}: GSDumpHeader block too small ({block})")
        (state_version, state_size, serial_offset, serial_size, crc, shot_w, shot_h, shot_offset, shot_size) = HEADER.unpack_from(data, pos)
        serial = bytes(view[pos + serial_offset:pos + serial_offset + serial_size]).decode("ascii", "replace")
        info.update({"format": "2.x", "state_version": state_version, "state_size": state_size, "serial": serial,
                     "crc": f"{crc:08x}", "embedded_screenshot": f"{shot_w}x{shot_h}" if shot_w else None,
                     "header_block": block})
        pos += block
    else:
        state_size = block
        info.update({"crc": f"{crc:08x}", "state_size": state_size, "state_version": None})
    pos += state_size
    regs0 = bytes(view[pos:pos + 8192])
    pos += 8192
    stats = Stats()
    stats.crtc_per_vsync.append({"vsync": 0, **CrtcState.parse(regs0)})
    paths = defaultdict(PathState)
    packets: Counter = Counter()
    while pos < len(data):
        tag = data[pos]
        pos += 1
        if tag == 0:  # Transfer
            path = data[pos]
            length = U32.unpack_from(data, pos + 1)[0]
            pos += 5
            packets[f"transfer {PATH_NAMES.get(path, path)}"] += 1
            stats.transfers[PATH_NAMES.get(path, path)] += 1
            stats.frame_counters["transfer_bytes"] += length
            gif_path = 3 if path == 0 else path
            stats.gif(view[pos:pos + length], gif_path, paths[gif_path])
            pos += length
        elif tag == 1:  # VSync
            field = data[pos]
            pos += 1
            packets["vsync"] += 1
            stats.vsync(field)
        elif tag == 2:  # ReadFIFO2
            size = U32.unpack_from(data, pos)[0]
            pos += 4
            packets["readfifo2"] += 1
            stats.readfifo_packets += 1
            stats.readfifo_bytes += size * 16
        elif tag == 3:  # Registers (privileged set at this vsync)
            block = bytes(view[pos:pos + 8192])
            pos += 8192
            packets["registers"] += 1
            stats.crtc_per_vsync.append({"vsync": stats.vsyncs + 1, **CrtcState.parse(block)})
        else:
            info["parse_error"] = f"unknown packet tag {tag} at offset {pos - 1}"
            break
    info["packets"] = dict(packets)
    info["stats"] = summarise(stats)
    return info


def summarise(s: Stats) -> dict:
    def top(counter: Counter, n: int = 40) -> dict:
        return {str(k): v for k, v in counter.most_common(n)}

    vsyncs = max(s.vsyncs, 1)
    kicks_per_vsync = [f["kicks"] for f in s.per_vsync]
    draws_per_vsync = [f["draws"] for f in s.per_vsync]
    idle = sum(1 for k in kicks_per_vsync if k == 0)
    # Frame-rate inference: how often does the displayed buffer (DISPFB FBP/DBY of an enabled circuit) change?
    shown = []
    for state in s.crtc_per_vsync:
        pm = state["PMODE"]
        key = tuple((state[f"DISPFB{i}"]["FBP"], state[f"DISPFB{i}"]["DBY"]) for i in (1, 2) if pm[f"EN{i}"])
        shown.append(key)
    flips = sum(1 for a, b in zip(shown, shown[1:]) if a != b)
    crtc_modes = Counter(CrtcState.summary(state) for state in s.crtc_per_vsync)
    fb_pages = s.render_targets | s.depth_targets
    fb_tex = sorted({f"0x{t:04x}" for t in s.texture_bases if (t >> 5) in fb_pages})
    return {
        "vsyncs": s.vsyncs,
        "fields": "".join(str(f) for f in s.fields[:120]),
        "gif_tags": s.gif_tags, "gif_flg": top(s.gif_flg), "transfers": top(s.transfers), "path_bytes": top(s.path_bytes),
        "path_tags": top(s.path_tags),
        "vertices": s.vertices, "vertex_kicks": s.kicks, "primitives_estimate": top(s.prim_count), "draws_estimate": s.draws,
        "per_vsync_kicks": kicks_per_vsync[:240], "per_vsync_draws": draws_per_vsync[:240],
        "per_vsync_image_bytes": [f["image_bytes"] for f in s.per_vsync][:240],
        "per_vsync_readbacks": [f["readbacks"] for f in s.per_vsync][:240],
        "idle_vsyncs": idle, "display_buffer_changes": flips, "display_buffer_sequence": [str(k) for k in shown[:120]],
        "avg_kicks_per_vsync": round(s.kicks / vsyncs, 1), "avg_draws_per_vsync": round(s.draws / vsyncs, 1),
        "prim_kicks": top(s.prim_kicks), "prim_flags": top(s.prim_flags, 60), "ctxt_used": top(s.ctxt_used),
        "reg_writes": top(s.reg_writes, 80), "ad_writes": top(s.ad_writes, 80),
        "frame_psm": top(s.frame_psm), "frame_fbw": top(s.frame_fbw), "frame_fbmsk": top(s.frame_fbmsk), "frame_fbp": top(s.frame_fbp),
        "zbuf_psm": top(s.zbuf_psm), "zbuf_zmsk": top(s.zbuf_zmsk), "zbuf_zbp": top(s.zbuf_zbp),
        "tex0_psm": top(s.tex0_psm), "tex0_cpsm": top(s.tex0_cpsm), "tex0_csm": top(s.tex0_csm), "tex0_cld": top(s.tex0_cld),
        "tex0_tfx": top(s.tex0_tfx), "tex0_tcc": top(s.tex0_tcc), "tex0_size": top(s.tex0_size, 30),
        "tex0_tbp_top": top(s.tex0_tbp, 30), "tex0_in_framebuffer_pages": fb_tex,
        "tex1": top(s.tex1), "tex1_mxl": top(s.tex1_mxl), "clamp": top(s.clamp),
        "alpha": top(s.alpha), "test": top(s.test),
        "bitblt_dpsm": top(s.bitblt_dpsm), "bitblt_spsm": top(s.bitblt_spsm), "trxdir": top(s.trxdir),
        "trxreg_sizes": top(s.trxreg_sizes, 40), "readback_pixels": s.readback_pixels, "local_copy_pixels": s.local_copy_pixels,
        "readfifo_packets": s.readfifo_packets, "readfifo_bytes": s.readfifo_bytes,
        "image_bytes": s.image_bytes, "image_bytes_by_psm": top(s.image_bytes_by_psm),
        "image_bytes_per_vsync_avg": round(s.image_bytes / vsyncs),
        "dthe": top(s.dthe), "dimx": top(s.dimx), "pabe": top(s.pabe), "fba": top(s.fba), "colclamp": top(s.colclamp),
        "scanmsk": top(s.scanmsk), "prmodecont": top(s.prmodecont), "texa": top(s.texa), "fogcol": top(s.fogcol),
        "fog_writes": s.fog_writes, "signal_finish_label": top(s.signal_finish_label),
        "render_target_pages": sorted(f"0x{p:03x}" for p in s.render_targets),
        "depth_target_pages": sorted(f"0x{p:03x}" for p in s.depth_targets),
        "crtc_modes": {k: v for k, v in crtc_modes.most_common(10)},
        "crtc_first": CrtcState.summary(s.crtc_per_vsync[0]) if s.crtc_per_vsync else None,
    }


def markdown(info: dict) -> str:
    st = info["stats"]
    lines = [f"### {Path(info['file']).name}", "",
             f"- container: {info['format']}, {info['size']:,} bytes, state version {info.get('state_version')}, "
             f"state {info.get('state_size', 0):,} bytes, serial {info.get('serial')!r}, CRC {info.get('crc')}",
             f"- packets: {info['packets']}",
             f"- vsyncs {st['vsyncs']}, GIF tags {st['gif_tags']}, vertex kicks {st['vertex_kicks']}, draws (est.) {st['draws_estimate']}, "
             f"avg kicks/vsync {st['avg_kicks_per_vsync']}, avg draws/vsync {st['avg_draws_per_vsync']}, idle vsyncs {st['idle_vsyncs']}, "
             f"display-buffer changes {st['display_buffer_changes']}",
             f"- per-vsync kicks: {st['per_vsync_kicks'][:64]}",
             f"- CRTC: {st['crtc_modes']}",
             f"- primitives: {st['primitives_estimate']}", f"- PRIM flags: {st['prim_flags']}",
             f"- FRAME PSM {st['frame_psm']}, FBW {st['frame_fbw']}, FBMSK {st['frame_fbmsk']}, FBP {st['frame_fbp']}",
             f"- ZBUF PSM {st['zbuf_psm']}, ZMSK {st['zbuf_zmsk']}, ZBP {st['zbuf_zbp']}",
             f"- TEX0 PSM {st['tex0_psm']}, CPSM {st['tex0_cpsm']}, CSM {st['tex0_csm']}, CLD {st['tex0_cld']}, TFX {st['tex0_tfx']}, TCC {st['tex0_tcc']}",
             f"- TEX0 sizes {st['tex0_size']}", f"- TEX0 bases inside render/depth pages: {st['tex0_in_framebuffer_pages']}",
             f"- TEX1 {st['tex1']}", f"- CLAMP {st['clamp']}", f"- ALPHA {st['alpha']}", f"- TEST {st['test']}",
             f"- BITBLTBUF DPSM {st['bitblt_dpsm']}, SPSM {st['bitblt_spsm']}, TRXDIR {st['trxdir']}, sizes {st['trxreg_sizes']}",
             f"- image bytes {st['image_bytes']:,} ({st['image_bytes_per_vsync_avg']:,}/vsync) by PSM {st['image_bytes_by_psm']}; "
             f"readbacks {st['readback_pixels']} px, ReadFIFO2 {st['readfifo_packets']} packets / {st['readfifo_bytes']} bytes; local copies {st['local_copy_pixels']} px",
             f"- DTHE {st['dthe']}, DIMX {st['dimx']}, PABE {st['pabe']}, FBA {st['fba']}, COLCLAMP {st['colclamp']}, SCANMSK {st['scanmsk']}, "
             f"PRMODECONT {st['prmodecont']}, TEXA {st['texa']}, FOGCOL {st['fogcol']}, FOG writes {st['fog_writes']}, {st['signal_finish_label']}",
             f"- register writes: {st['reg_writes']}", f"- A+D targets: {st['ad_writes']}", ""]
    return "\n".join(lines)


def table(results: list[dict]) -> str:
    """One markdown table, one column per dump: the per-scene comparison the S7 findings use."""
    def rendered_frames(st: dict) -> int:
        return sum(1 for k in st["per_vsync_kicks"] if k)

    def per_frame(st: dict, value: float) -> str:
        frames = rendered_frames(st) or 1
        return f"{value / frames:,.0f}"

    def top(counter: dict, n: int = 3) -> str:
        items = list(counter.items())[:n]
        return ", ".join(f"{k} ×{v}" for k, v in items) if items else "none"

    def rate(st: dict) -> str:
        kicks = st["per_vsync_kicks"]
        if not kicks:
            return "?"
        busy = [k > 0 for k in kicks]
        if all(busy):
            return "every vsync (60 Hz)"
        if all(a != b for a, b in zip(busy, busy[1:])):
            return "alternate vsyncs (30 Hz)"
        return f"{sum(busy)} of {len(busy)} vsyncs"

    rows: list[tuple[str, list[str]]] = []
    names = [Path(r["file"]).stem for r in results]
    sts = [r["stats"] for r in results]

    def add(label: str, fn) -> None:
        rows.append((label, [fn(r, st) for r, st in zip(results, sts)]))

    add("dump size (bytes)", lambda r, st: f"{r['size']:,}")
    add("vsyncs / rendered frames", lambda r, st: f"{st['vsyncs']} / {rendered_frames(st)}")
    add("update rate", lambda r, st: rate(st))
    add("display-buffer changes", lambda r, st: str(st["display_buffer_changes"]))
    add("vertex kicks per frame", lambda r, st: per_frame(st, st["vertex_kicks"]))
    add("state-change draws per frame (est.)", lambda r, st: per_frame(st, st["draws_estimate"]))
    add("GIF tags per frame", lambda r, st: per_frame(st, st["gif_tags"]))
    add("primitives", lambda r, st: top(st["primitives_estimate"], 6))
    add("PRIM flags (top)", lambda r, st: top(st["prim_flags"], 4))
    add("FRAME PSM", lambda r, st: top(st["frame_psm"]))
    add("FRAME FBW", lambda r, st: top(st["frame_fbw"]))
    add("FRAME FBMSK", lambda r, st: top(st["frame_fbmsk"]))
    add("render-target pages", lambda r, st: f"{len(st['render_target_pages'])}: " + " ".join(st["render_target_pages"][:8]) + (" …" if len(st["render_target_pages"]) > 8 else ""))
    add("ZBUF PSM / ZMSK", lambda r, st: top(st["zbuf_psm"]) + " / " + top(st["zbuf_zmsk"]))
    add("TEX0 PSM", lambda r, st: top(st["tex0_psm"], 5))
    add("CLUT PSM / CSM / CLD", lambda r, st: top(st["tex0_cpsm"]) + " / " + top(st["tex0_csm"]) + " / " + top(st["tex0_cld"]))
    add("TEX0 TFX / TCC", lambda r, st: top(st["tex0_tfx"]) + " / " + top(st["tex0_tcc"]))
    add("TEX0 sizes", lambda r, st: top(st["tex0_size"], 4))
    add("texture bases in RT/Z pages", lambda r, st: str(len(st["tex0_in_framebuffer_pages"])))
    add("TEX1 (filter, MXL)", lambda r, st: top(st["tex1"], 2))
    add("CLAMP", lambda r, st: top(st["clamp"]))
    add("ALPHA formulas", lambda r, st: top(st["alpha"], 4))
    add("TEST", lambda r, st: top(st["test"], 4))
    add("uploads (TRXDIR)", lambda r, st: top(st["trxdir"]))
    add("upload sizes (top)", lambda r, st: top(st["trxreg_sizes"], 4))
    add("image bytes per vsync", lambda r, st: f"{st['image_bytes_per_vsync_avg']:,}")
    add("readback pixels / ReadFIFO", lambda r, st: f"{st['readback_pixels']} / {st['readfifo_packets']}")
    add("local copies (pixels)", lambda r, st: f"{st['local_copy_pixels']:,}")
    add("DTHE / PABE / FBA / COLCLAMP", lambda r, st: f"{top(st['dthe'])} / {top(st['pabe'])} / {top(st['fba'])} / {top(st['colclamp'])}")
    add("PRMODECONT / FOGCOL / FOG writes", lambda r, st: f"{top(st['prmodecont'])} / {top(st['fogcol'], 2)} / {st['fog_writes']}")
    add("TEXA", lambda r, st: top(st["texa"], 2))
    add("CRTC", lambda r, st: list(st["crtc_modes"].keys())[0].split("; RC1")[0] if st["crtc_modes"] else "?")
    header = "| metric | " + " | ".join(names) + " |"
    sep = "|---|" + "---|" * len(names)
    body = "\n".join(f"| {label} | " + " | ".join(v.replace("|", "/") for v in values) + " |" for label, values in rows)
    return header + "\n" + sep + "\n" + body + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dumps", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, help="write the full statistics to this file")
    parser.add_argument("--markdown", action="store_true", help="print a markdown summary per dump")
    parser.add_argument("--table", action="store_true", help="print one markdown table comparing all dumps")
    args = parser.parse_args(argv)
    results = [parse_dump(p) for p in args.dumps]
    if args.json:
        args.json.write_text(json.dumps(results, indent=1) + "\n", encoding="utf-8")
    if args.table:
        print(table(results))
    elif args.markdown:
        for info in results:
            print(markdown(info))
    elif not args.json:
        print(json.dumps(results, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
