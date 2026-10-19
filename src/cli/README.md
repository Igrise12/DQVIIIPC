# src/cli/

The command-line front end (`dq8ctl`). Exposes the same operations as the launcher (verify, setup, build, run, diagnose) for scripting, CI and headless use. Links `dq8::core` only (architecture.md §13.2) and has no dependencies beyond it; argument parsing is hand-rolled.

Commands (M1 part 1):

| Command | What it does |
|---|---|
| `verify <iso> [--regions DIR] [--json]` | Identify the disc from its boot file and cross-check it against the region profiles. |
| `extract <iso> [--out DIR] [--regions DIR] [--force] [--emit-profile] [--no-verify-hashes] [--json]` | Copy the boot file, overlays and IOP modules into `DIR/<profile id>/` (default `baserom/`). Re-runs skip files that are already present and intact. `--emit-profile` prints the profile rows with the observed sizes and hashes on stdout (status lines go to stderr) so a developer can paste them into `data/regions/`. |
| `info <iso>` / `info <boot elf>` | Volume facts, SYSTEM.CNF, the listing with LBA and size, the path-table cross-check; or the ELF header and its segments. |

Exit status: 0 success, 1 the image does not match a profile or an extracted file's hash differs, 2 usage or I/O error. `--regions` defaults to `data/regions` under the current directory, under an ancestor of the executable (the build tree) or `share/dq8/regions` beside it.

`--json` prints one object on stdout: `{"ok": true, ...}` or `{"ok": false, "error": {"code": "<ErrorCode name>", "message": "..."}}`. That object, and the `ErrorCode` names in it, are the contract the launcher will consume; add fields freely, never rename or remove them.
