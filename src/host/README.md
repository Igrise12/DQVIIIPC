# src/host/

The game executable (dq8-game). Wires platform, runtime, the locally built game module and the enabled features together. Contains no logic of its own beyond start-up and shutdown.

M2 (`main.cpp`, `json_writer.h`): start-up only, then one run of the main unit until the guest stops.

```bash
dq8-game --iso <image> --module-dir <dir> [--regions DIR] [--stop-at-first-syscall] [--max-dispatches N]
         [--trace-dispatch] [--trap-mmio] [--json]
dq8-game --elf <file> --module-dir <dir> [--window BASE:END] [--resident ID] [run options]
```

- `--iso`: `core::verify_image()` against the region profiles, then the seven unit specs from `game::unit_specs()`, then `rt::load_modules()` from `--module-dir` (`libdq8_unit_<unit>.so`, `.dylib`, or `dq8_unit_<unit>.dll`). Every missing module and every descriptor problem is listed before anything runs. Then the boot ELF is read from the image and placed in guest memory, the main table goes to the dispatcher and the six overlay tables to the overlay manager (none resident), and the main unit runs from the ELF entry.
- `--elf` (developer mode): any MIPS ELF and every `dq8_unit_*` library in `--module-dir`. Each descriptor says what its unit is; checked are its own consistency, that its name matches its file name, exactly one main unit whose entry is the ELF's, overlays inside `--window`, and the main table outside it. `--resident ID` makes an overlay resident at start.
- Exit status: 0 stopped at a syscall (or a by-name kernel call) with `--stop-at-first-syscall`; 1 any other guest stop; 2 usage, I/O, disc verification or module validation.
- `--json` prints one object: `ok`, `mode`, `profile` (iso) or `elf_path`, `elf` (entry, segments), `overlay_window`, `resident`, `modules` (unit, path, kind, overlay_id, profile_id, source_sha1, generator, entry_point, table_base, table_end, slots, entries), `stop` (reason, message, pc, address, and for kernel calls `call_kind`, `name`, `number` such as `"0x3C"` or `"-0x1A"`, `args.a0..a3` as 32-bit hex; `last_branch`), `dispatches`, `context` (pc, hi, lo, sa, `gpr.<name>` as the low 64 bits, `gpr_hi` for registers whose high half is non-zero), `mmio` (first access per address with region, pc, count) and `exit_code`. On failure: `{"ok": false, "error": {"code", "message"}, "problems": [...], "exit_code": 2}`.
- `--trace-dispatch` writes one line per dispatch to stderr; in text mode the first access to each device address is reported on stderr as it happens.
