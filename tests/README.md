# tests/

| Directory | Label | Needs game data | Runs in CI |
|---|---|---|---|
| `unit/` | `unit` | No | Yes |
| `local/` | `local` | Yes: set `DQ8_TEST_ISO=/path/to/your.iso`; every test skips itself otherwise | No |
| `e2e/` | `e2e` | No: hand-assembled MIPS fixtures; needs `-DDQ8_BUILD_RECOMP=ON -DDQ8_BUILD_E2E=ON` (Linux) | Yes, job `recomp-e2e` |
| `oracle/` | `oracle` | Yes (differential tests against the reference interpreter; none yet) | No |
| `fixtures/` | | Synthetic inputs only; nothing from a disc | |

`unit/support/` holds the synthetic-input builders: `IsoBuilder` writes small, valid ISO 9660 images (both path tables, both-byte-order fields, nested directories, `;1` identifiers, files spanning sectors) and records where each field landed so a test can corrupt one; `fake_boot_elf()` builds an ELF32 header over seeded random bytes; `build_mwo3()` builds an `MWo3` overlay container from an `Mwo3Spec` of our own sizes and addresses over seeded bytes (`put_le32()` corrupts one word); `seeded_bytes()` is the only source of "data"; `mips_asm.h` has constexpr encoders for the MIPS instructions synthetic code units need plus a small `Program` builder (also used by `tests/e2e/`); `process_child.cpp` builds `dq8_test_child`, a portable stand-in program for the process-runner tests that also plays `ps2_recomp` when given one `.toml` argument. `ps2_call_list.h` is a temporary verbatim copy of the emitter's call list for `src/core/recomp` until `include/dq8/abi/ps2recomp/ps2_call_list.h` is merged. Keep it that way: a fixture that resembles disc content is a policy violation (architecture.md §14), not a test asset.

`unit/support/machine_unit.*` is a synthetic multi-thread guest program in the emitter's shape: threads, semaphores, a vblank handler, the GS registers, a GIF DMA sprite and a fileio read. `dq8_machine_inputs` writes its ELF and a synthetic ISO. `host.dq8_game_machine_synthetic` runs it under `dq8-game --headless --gs trace --vblanks 10` and compares golden GS-stream and RAM hashes. It registers once `dq8-game` has the M3 machine.

`local/` checks the disc only against facts the region profile already records (names, sizes, hashes, header values); `test_real_overlays.cpp` parses the six overlays from the image and checks `64 + text + data + bss == segment memsz` and the profile rows.

`local.boot_to_title` is M3a's acceptance 1. It runs `dq8-game` on your disc with our authored input, `local/input/boot-to-title.dq8in`, until the title screen. The game modules come from `DQ8_TEST_MODULE_DIR`, by default `work/recomp/modules/SLUS_21207`. The test checks the exit code and the events: TITLE resident through the hook, mode 4, no stop. The events file stays in the build tree.

```bash
ctest --preset unit-dev-clang                                   # CI's suite
DQ8_TEST_ISO="$DQ8_ISO" ctest --preset local-dev-clang          # against your disc; writes only to a temp dir
./build/dev-clang/tests/unit/dq8_unit_tests '[iso]'             # one tag, straight through Catch2
```
