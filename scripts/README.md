# scripts/

Developer and setup helper scripts (environment checks, setup wrappers, hash checks). Thin wrappers only: real logic belongs in src/core or tools/.

`check_no_game_data.py` is the no-game-data gate (Docs/architecture.md §14): `--staged` (pre-commit), `--tree`
(CI), `--archive`, paths, `--deep` (byte windows of the local disc files) and `--release` (an install tree or an
unpacked AppImage: host executables and libraries allowed, MIPS code refused). Files under `data/maps/` must be
published maps in exactly the format of `Docs/formats/function-map.md`. Its tests are
`tests/unit/test_published_map_tools.py`.

`ci_test_annotations.py BUILD_DIR` turns the last ctest run's failed tests into GitHub Actions error annotations
(each test's name and the end of its output): CI job logs need a GitHub sign-in, annotations do not. The workflows
run it after a failed unit-test step.
