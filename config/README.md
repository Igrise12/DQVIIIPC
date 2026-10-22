# config/

Authored configuration: recompiler templates, function maps we wrote, the settings schema. Addresses, sizes and names are fine here. Instruction bytes, data values and text from the game are not.

- `settings/schema.toml`: every user-facing setting (ADR-0006; format in Docs/milestones/M5-launcher.md). Keys are
  added, never renamed or reused; defaults reproduce M4's `--window` behaviour.
- `settings/strings/<language>.toml`: the labels, descriptions, choice texts and page names, in our own words.
