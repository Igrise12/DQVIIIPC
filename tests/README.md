# tests/

| Directory | Label | Needs game data | Runs in CI |
|---|---|---|---|
| `unit/` | `unit` | No | Yes |
| `local/` | `local` | Yes: set `DQ8_TEST_ISO=/path/to/your.iso`; every test skips itself otherwise | No |
| `oracle/` | `oracle` | Yes (differential tests against the reference interpreter; none yet) | No |
| `fixtures/` | | Synthetic inputs only; nothing from a disc | |

`unit/support/` holds the synthetic-input builders: `IsoBuilder` writes small, valid ISO 9660 images (both path tables, both-byte-order fields, nested directories, `;1` identifiers, files spanning sectors) and records where each field landed so a test can corrupt one; `fake_boot_elf()` builds an ELF32 header over seeded random bytes; `seeded_bytes()` is the only source of "data". Keep it that way: a fixture that resembles disc content is a policy violation (architecture.md §14), not a test asset.

```bash
ctest --preset unit-dev-clang                                   # CI's suite
DQ8_TEST_ISO="$DQ8_ISO" ctest --preset local-dev-clang          # against your disc; writes only to a temp dir
./build/dev-clang/tests/unit/dq8_unit_tests '[iso]'             # one tag, straight through Catch2
```
