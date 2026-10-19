# Spikes

A spike is a short, time-boxed investigation that answers one question the plan depends on
(project-brief.md §11, "M0 spikes"). Each spike has a brief in this directory. A spike session reads its
brief, does the work, and appends a `## Findings` section to the same file. Nothing else in the repository
changes during a spike unless the brief says so (first-party helper scripts under `tools/` are allowed).

| Spike | Question | Depends on |
|---|---|---|
| [S0](S0-rea-capability.md) | Can REA (Ghidra MCP/CLI) open our R5900 ELF at all? | REA installed |
| [S1](S1-ps2recomp-build.md) | Does PS2Recomp build here, and what does a game-module build cost? | nothing |
| [S2](S2-ghidra-headless-export.md) | Can headless Ghidra + the EE extension analyse the stripped ELF and export a function map? | nothing |
| [S3](S3-overlay-loader.md) | How does the game load `BIN/*.BIN` overlays into `0x00461C80`? | S2 import |
| [S4](S4-data-archive-index.md) | What is the `DATA.HD6` / `DATA.DAT` archive layout? | S2 import |
| [S5](S5-parallel-gs-standalone.md) | Does paraLLEl-GS build and replay our GS dumps? | S5b: S7 dumps |
| [S6](S6-mvi-movie-format.md) | What is the `.MVI` movie container? | S6b: S2 import |
| [S7](S7-pcsx2-headless-baseline.md) | What are the real frame rate, resolution and GS usage? | nothing |

## Inputs

The coordinator extracts spike inputs once, into the git-ignored `baserom/raw/`:

```bash
set -a; . tools/local.env; set +a
7z x -y -obaserom/raw "$DQ8_ISO" SLUS_212.07 SYSTEM.CNF DATA.HD6 DATA2.HD6 BIN MODULES
```

Tool paths come from `tools/local.env` (copy `tools/local.env.example`). Scratch work goes in `work/<spike>/`
(git-ignored) or the session scratchpad. Movies and the `.DAT` archives are never extracted; stream them with
`7z e -so "$DQ8_ISO" <path> | ...`.

## The rule that overrides everything else

**No game-derived content may be committed** (project-brief.md §9, architecture.md §14). For a findings doc that means:

- **Never:** hexdumps (not even one line), disassembly or decompiler output, strings tables or any game text,
  file names from inside archives, generated C++, screenshots or video of game frames, GS dumps, save states,
  extracted files, key or constant *values* read from the binary.
- **Allowed:** addresses, sizes, offsets, counts, histograms, magic identifiers (4-byte tags), hashes,
  field-layout tables you authored, names you invented, timings, tool output that contains none of the above.
- Keep raw evidence under `work/` and cite it by path. Before committing, run
  `python3 -I scripts/check_no_game_data.py --staged` (once it exists) and read the diff yourself.

## Findings template

Append this to the brief when done. Every fact carries an evidence pointer (an address, a count, a command, a
path under `work/`).

```markdown
## Findings

**Session:** <date>, <time spent>. **Stop reason:** done | time box | blocked (<why>).

### Facts
- <fact> — evidence: <pointer>

### What changes in the docs
- project-brief.md §<n>: <edit>
- architecture.md §<n>: <edit>

### Decision impact
- D<n> / ADR-<nnnn>: <recommendation and why>

### Local artifacts (git-ignored)
- work/<spike>/<file>: <what it is>

### Open questions
- <question> → <which spike or milestone answers it>
```

## Time boxes

Each brief states its time box in sessions. When it is reached, write the findings you have, state what is
missing, and stop. A partial answer with evidence beats a complete answer without.
