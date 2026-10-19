# S0: Can REA open our R5900 ELF?

**Time box:** 1 short session. **Depends on:** REA installed and registered (`rea --version`, `claude mcp list`).

## Why

[REA](https://github.com/morluto/rea) ("Reverse Engineer Anything", MIT) is an MCP server and CLI that drives
an existing Ghidra install. If it can open `SLUS_212.07` with the Emotion Engine language, later spikes
(S3, S4, S6b) can query functions, xrefs and bytes interactively through MCP instead of writing a Ghidra script
for every question. REA's documented MIPS support covers MIPS32/MIPS32R2 ELFs only, and it has a console
profile for PSP but none for PS2. Our ELF is `e_flags=0x20924000` (MIPS **ARCH_3**, no ABI-flags record), so
refusal is the expected outcome. This spike establishes the fact either way and records what a PS2 profile
would need.

## Inputs and tools

- `baserom/raw/SLUS_212.07`, `baserom/raw/BIN/*.BIN`
- `rea` 6.7.0 (`npm install --global rea-agents@6.7.0`), `GHIDRA_INSTALL_DIR` = `DQ8_GHIDRA_INSTALL_DIR`
  from `tools/local.env` (Ghidra 12.1.4 with `ghidra-emotionengine-reloaded` 2.1.38 installed), JDK 27 on PATH.
- REA docs to read first: `docs/installation.md`, `docs/ghidra-provider.md`, `docs/ghidra-mips.md`,
  `docs/ghidra-psp.md`, `docs/cli.md`.

## Steps

```bash
set -a; . tools/local.env; set +a; export GHIDRA_INSTALL_DIR="$DQ8_GHIDRA_INSTALL_DIR"
mkdir -p work/rea
rea doctor --provider ghidra --json > work/rea/doctor.json
rea capabilities --json > work/rea/capabilities.json
# 1. Default admission (expected: refused with a MIPS-lane error code)
( time rea analyze baserom/raw/SLUS_212.07 --provider ghidra --json \
    --snapshot work/rea/slus21207-default.json ) > work/rea/analyze-default.json 2> work/rea/analyze-default.err
# 2. Forced EE language (expected: either a typed refusal, or an import with language_id r5900:LE:32:default)
( time REA_GHIDRA_LANGUAGE_ID=r5900:LE:32:default REA_GHIDRA_COMPILER_SPEC_ID=default \
    rea analyze baserom/raw/SLUS_212.07 --provider ghidra --json \
    --snapshot work/rea/slus21207-r5900.json ) > work/rea/analyze-r5900.json 2> work/rea/analyze-r5900.err
```

If step 2 is admitted:

```bash
# Does the EE decoder work? Count MMI/COP2 mnemonics in a few functions' assembly (output stays in work/).
rea instructions baserom/raw/SLUS_212.07 --provider ghidra --address=0x00100008 --json > work/rea/instr-entry.json
rea search baserom/raw/SLUS_212.07 --provider ghidra --kind procedures "" --json > work/rea/procedures.json
```
Record the reported `language_id`, procedure count, cold-start seconds, and whether MMI (`pextlw`, `paddw`,
`pcpyld`, …) or COP2 (`vadd`, `qmfc2`, `vcallms`, …) mnemonics appear anywhere in the sampled assembly. Then
test the MCP path in a fresh Claude Code session: `open_binary` with `provider_id: "ghidra"`, `binary_overview`,
one `analyze_function` at `0x00100008`, `close_binary` with `snapshot_path`.

If step 2 is refused: record the exact error code and message, and try the overlay body as a raw binary
(`--target-format raw-binary --architecture ...`): REA lists no MIPS architecture there, so this should also be
refused; record it.

## Success criteria

A clear verdict with error codes or counts for: default admission, forced-language admission, MCP path,
raw-binary path. A recommendation for S3/S4/S6b: "REA MCP is usable for interactive queries" or
"headless Ghidra scripts only".

## Deliverables

- Findings below.
- If refused: a short section "What a PS2/EE profile in REA would need", modelled on REA's `docs/ghidra-psp.md`
  (ELF admission: `e_machine` MIPS + `ARCH_3` + no ABI record; extension `ghidra-emotionengine-reloaded`
  content-fingerprinted under `Ghidra/Extensions`; language `r5900:LE:32:default`, compiler `default`;
  loader `ElfLoader`). This becomes an upstream issue text.
- tech-stack.md §7 row for REA updated with the verdict.

## Must not commit

Anything under `work/rea/` (snapshots and evidence bundles contain pseudocode and bytes), assembly listings,
procedure names from the binary.

## Findings

**Session:** 2026-10-19, about 15 minutes (CLI path; the MCP path uses the same `open_binary` admission and was not exercised separately). **Stop reason:** done.

### Facts
- REA 6.7.0 installed globally with npm (Node 24.21); `rea setup --client claude_code` registered the `rea` MCP server in `~/.claude.json` with `GHIDRA_INSTALL_DIR` pointing at the MGS2_Undub Ghidra 12.1.4 install and `JAVA_HOME` at JDK 27, and installed the REA skill under `~/.claude/skills/reverse-engineer-anything`. Evidence: `~/.claude.json` `mcpServers.rea`.
- `rea doctor --provider ghidra` is **healthy**: Ghidra 12.1.4 found, headless launcher found, native decompiler matches linux/x64, JDK 27 "accepted a 64-bit full JDK 21 or newer". Evidence: `work/rea/doctor.json`.
- **Default admission is refused** in 1.1 s, before Ghidra starts: `code: capability_unavailable`, `category: unsupported_provider`, Ghidra rejection `architecture_unsupported / Machine-specific MIPS variants, including PSP Allegrex, need a separately supported processor profile.`. Hopper, IDA, Kuna and JADX are unconfigured, and REA's Hopper adapter does not admit MIPS anyway. Evidence: `work/rea/analyze-default.json`, `work/rea/s0.log`.
- **Forcing the language** (`REA_GHIDRA_LANGUAGE_ID=r5900:LE:32:default REA_GHIDRA_COMPILER_SPEC_ID=default`) produces the identical refusal: admission runs before the language override is consulted. Oddity: this run exited with status 0 while printing the same error payload (the default run exited 1). Evidence: `work/rea/analyze-r5900.json`.
- **Raw-binary probe** on an overlay body (`--target-format raw-binary --architecture mips`) is refused: `VALIDATION_ERROR / Invalid option: expected one of "6502"|"x86"|"x86_64"|"arm"|"arm64"|"m68k"

Details: [
  {
    "code": "invalid_value",
    "values": [
      "6502",
      "x86",
      "x86_64",
      "arm",
      "arm64",
      "m68k"
    ],
    "path": [
      "architecture"
    ],
    "message": "Invalid option: expected one of \"6502\"|\"x86\"|\"x86_64\"|\"arm\"|\"arm64\"|\"m68k\""
  }
]`. REA's raw-binary lane lists x86, x86_64, arm, arm64, 6502 and m68k only. Evidence: `work/rea/raw-viewer.json`.
- The refusal is REA's own ELF-profile check (its MIPS lanes admit MIPS32/32R2 ELFs with or without an ABI record; ours is `e_flags=0x20924000`, MIPS III, no ABI record). Ghidra itself with the Emotion Engine extension is unaffected.

### What changes in the docs
- tech-stack.md §7: REA row records the verdict; analysis tooling for this project is headless Ghidra (`tools/ghidra/`, spike S2).
- Spike briefs S3, S4, S6b: the "or REA if S0 passed" option does not apply; use the Ghidra project copies.

### Decision impact
- No decision in brief §14 depends on this. REA stays installed and registered: it is useful for host-side binaries (our own executables, third-party libraries) and would become useful for the game if upstream adds a PS2 profile.

### What a PS2/EE profile in REA would need (for an upstream feature request, modelled on `docs/ghidra-psp.md`)
- Admission: ELF32 little-endian `ET_EXEC`, `e_machine` MIPS, `e_flags` architecture field `ARCH_3` (observed `0x20924000`), no `.MIPS.abiflags` section; optionally the `SYSTEM.CNF`-style boot-file name pattern `S[CL][UEP][SM]_xxx.xx` as a hint only.
- Extension: `ghidra-emotionengine-reloaded` under `Ghidra/Extensions`, content-fingerprinted like `ghidra-allegrex` is for PSP.
- Loader `ElfLoader`, language `r5900:LE:32:default`, compiler `default`; the handshake confirms the language before serving results.
- Known limitation to document: overlays loaded at run time into a shared window (this game) are separate programs; REA's single-target session model handles them one file at a time.
- Not filed upstream yet; that is the project owner's call.

### Local artifacts (git-ignored)
- `work/rea/doctor.json`, `capabilities.json`, `analyze-default.json`, `analyze-r5900.json`, `raw-viewer.json`, `s0.sh`, `s0.log`.

### Open questions
- None blocking. Whether to open the upstream feature request → project owner.
