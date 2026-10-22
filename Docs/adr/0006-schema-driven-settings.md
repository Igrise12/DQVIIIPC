# ADR-0006: Settings come from a schema; the launcher renders them

| | |
|---|---|
| **Status** | **Accepted** 2026-10-22 by the project owner (M5 planning) |
| **Related** | architecture.md §9, §10, §11.2, §17; ADR-0010; Docs/milestones/M5-launcher.md |

## Context

Until M4 every runtime option of `dq8-game` is a command-line flag and nothing is persisted. M5's launcher needs a
settings UI, and features to come (scaling options, widescreen, enhancements) each bring settings of their own.
Architecture §9 designed a schema, layered values and TOML storage, and §17 requires that adding a setting needs no
launcher code.

## Decision

- Every user-facing setting is declared once in a **settings schema** (`config/settings/schema.toml`): key, type,
  range or choices, default, label and description, category, advanced flag, `requires_restart`, `hot_reload`,
  dependencies and owner.
- Values are **layered**: schema defaults, then the user file, then the profile file, then `--set key=value`; a
  command-line flag of `dq8-game` still overrides all of them.
- Values are stored as **TOML** with a `schema_version`, migrations, and unknown keys preserved; the file lives in the
  user configuration directory, or next to the executable when a `portable` marker file is present.
- The schema, the store and the mapping from settings to `dq8-game` options live in `libdq8core`; the launcher renders
  its settings pages from the schema, and `dq8ctl settings get|set|list` uses the same API.

## Consequences

- A new setting is a schema entry plus the code that reads it; no launcher change.
- `dq8-game` gains `--config FILE` and `--set key=value`; existing flags keep working, so scripted runs and tests do
  not change.
- Default settings reproduce M4's behaviour exactly, so the M3/M4 goldens stay valid.
