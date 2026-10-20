# tests/

| Directory | Label | Needs game data | Runs in CI |
|---|---|---|---|
| `unit/` | `unit` | No | Yes |
| `local/` | `local` | Yes: set `DQ8_TEST_ISO=/path/to/your.iso`; every test skips itself otherwise | No |
| `oracle/` | `oracle` | Yes (differential tests against the reference interpreter; none yet) | No |
| `fixtures/` | | Synthetic inputs only; nothing from a disc | |

`unit/support/` holds the synthetic-input builders: `IsoBuilder` writes small, valid ISO 9660 images (both path tables, both-byte-order fields, nested directories, `;1` identifiers, files spanning sectors) and records where each field landed so a test can corrupt one; `fake_boot_elf()` builds an ELF32 header over seeded random bytes; `build_mwo3()` builds an `MWo3` overlay container from an `Mwo3Spec` of our own sizes and addresses over seeded bytes (`put_le32()` corrupts one word); `seeded_bytes()` is the only source of "data"; `mips_asm.h` has constexpr encoders for the MIPS instructions synthetic code units need plus a small `Program` builder (also used by `tests/e2e/`); `process_child.cpp` builds `dq8_test_child`, a portable stand-in program for the process-runner tests that also plays `ps2_recomp` when given one `.toml` argument. `ps2_call_list.h` is a temporary verbatim copy of the emitter's call list for `src/core/recomp` until `include/dq8/abi/ps2recomp/ps2_call_list.h` is merged. Keep it that way: a fixture that resembles disc content is a policy violation (architecture.md §14), not a test asset.

`local/` checks the disc only against facts the region profile already records (names, sizes, hashes, header values); `test_real_overlays.cpp` parses the six overlays from the image and checks `64 + text + data + bss == segment memsz` and the profile rows.

```bash
ctest --preset unit-dev-clang                                   # CI's suite
DQ8_TEST_ISO="$DQ8_ISO" ctest --preset local-dev-clang          # against your disc; writes only to a temp dir
./build/dev-clang/tests/unit/dq8_unit_tests '[iso]'             # one tag, straight through Catch2
```
