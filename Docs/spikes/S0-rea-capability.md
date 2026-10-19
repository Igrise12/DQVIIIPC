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
