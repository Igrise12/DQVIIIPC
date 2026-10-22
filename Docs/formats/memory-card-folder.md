# Memory-card folder

A memory-card folder is our own format: one PS2 memory card (8 MB) kept as plain files on the host (ADR-0016).
`dq8-game` serves it to the game through its native memory-card server; nothing in it comes from the disc. What a
folder holds is what the game wrote: the player's own saves. Never commit one.

The runtime keeps the card in memory (`rt::mc::MemoryCard`, `src/runtime/io/memcard/memory_card.h`) and that copy
alone decides what the game sees. The folder is read once when `dq8-game` starts and written behind the game's
back (`open_folder_card_store`, `src/runtime/io/memcard/memory_card_store.h`). Its tests are in
`tests/unit/test_memcard.cpp`.

## Where

```
<save-dir>/<profile>/card1/     port 0 (--memory-card)
<save-dir>/<profile>/card2/     port 1 (--memory-card2)
```

- `<save-dir>` is `--save-dir DIR`. With `--window` the default is the user data directory: `%APPDATA%\DQ8-Native`
  on Windows, `~/Library/Application Support/DQ8-Native` on macOS, `$XDG_DATA_HOME/dq8-native` (default
  `~/.local/share/dq8-native`) elsewhere. Headless runs, and `--save-dir none`, use an in-memory card instead, so
  scripted runs never touch user files.
- `<profile>` is the region profile's id (`SLUS_21207`), or `dev` in developer mode (`--elf`).
- `--memory-card STATE` (`unformatted`, the default, `formatted` or `none`) says what a port holds when its folder
  has no `card.toml` yet; once the folder exists, the folder wins. `none` leaves the port empty.

## Layout

```
card1/
  card.toml            the card: format version and whether it is formatted
  .dir.toml            the root directory: its times and its entries in card order
  BASLUS-21207dq8_1/   a directory of the card
    .dir.toml          its times and entries
    icon.sys           a file of the card, byte for byte
    ...
  .backup/             replaced and deleted files, three generations
  .tmp/                writes in progress
```

Every directory of the card is a host directory with a `.dir.toml`; every file is a host file holding exactly the
card file's bytes. Names starting with `.` are ours: the card's own names never start with one (see Names).

### `card.toml`

```toml
format = "dq8-memory-card"
version = 1
formatted = true
```

A folder without `card.toml` is a new card. An unformatted card has only `card.toml` (and perhaps `.backup/`).
`version` other than 1 refuses to load.

### `.dir.toml`

```toml
version = 1
attr = 0x8427
created = "2005-11-15T12:00:00"
modified = "2005-11-15T12:00:30"

[[entry]]
name = "BASLUS-21207dq8_1"
kind = "dir"
attr = 0x8427
created = "2005-11-15T12:00:00"
modified = "2005-11-15T12:00:00"

[[entry]]
kind = "free"

[[entry]]
name = "a:b"
file = "a%3Ab"
kind = "file"
attr = 0x8497
created = "2005-11-15T12:00:10"
modified = "2005-11-15T12:00:20"
```

- The entries are in **card order**, which is the order the game's directory listings return (after `.` and `..`,
  which every directory lists first and the file does not store).
- `kind = "free"` is a slot a deleted entry left; the next new entry takes the first free slot, as on a real card.
  Keeping them keeps listing order the same after a reload.
- `attr` is the card's attribute word (`0x8427` a directory, `0x8497` a closed file); `created` and `modified`
  are the card's timestamps, from the virtual clock (`--rtc`), never from the host. Host modification times are
  ignored.
- `name` is the card name; `file` is the host file name, given only when it differs (Names). Bytes outside
  printable ASCII are written as `\u00XX` and read back as that byte.
- A file's size is its host file's size.

## Names

A card name is at most 31 bytes, without `/`, `\`, `*` or `?`, and not `.` or `..`. Its host file name is the
same name unless that is unsafe on some host file system; then each unsafe byte becomes `%XX` (upper-case hex):
`%` itself, bytes below 0x20 or above 0x7E, `<>:"/\|?*`, a leading or trailing `.` or space, a leading `~`, and the
first byte of a Windows device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`, with or without an
extension). Two names of one directory that differ only in case get distinct host names (`~2`, `~3`, ... appended
to the later one), recorded in `file`.

## Writing

- The game's changes reach the folder at the points where a real card has them on flash: a file closed or flushed
  after writes, an entry created, deleted or renamed, times or attributes set, the card formatted or unformatted.
  Each is copied when it happens and written by one I/O thread, in the game's order, so the folder always holds a
  state the card went through.
- Every file and every `.dir.toml` is written to `.tmp/` first, flushed to disk, then renamed over its target
  (`MoveFileExW` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` on Windows): a write interrupted at any
  point leaves the old content. A file's metadata is written after its bytes.
- Before a file is replaced by different bytes or deleted, its old content goes to `.backup/<path>.1`, and older
  backups move to `.2` and `.3`; the fourth is dropped. Formatting and unformatting back up every file first.
- Disk speed and disk errors never reach the game: the in-memory card has already answered. A failed write is
  reported on stderr, and `dq8-game` waits for the last writes when the run ends.

## Reading

- `.tmp/` is cleared (unfinished writes of an earlier run).
- `card.toml` says whether the card is formatted; a formatted card's tree is read from the `.dir.toml` files.
- An entry the metadata lists but the folder lacks is dropped, with a warning.
- Files and directories the metadata does not list (copied in by hand, or written just before a crash) are
  appended in name order, their card name decoded from `%XX`, their times the directory's own.
- A `.dir.toml` that does not parse is reported, and its directory read in name order.
- A card holding more than 8 MB loads but reports no free space.

## Capacity

The card counts 1 KiB clusters as mcman does: a file takes `ceil(size / 1024)` clusters, a directory one cluster
per two entries (counting `.`, `..` and free slots). A freshly formatted card reports 8,000 free clusters (8,001
allocatable, one taken by the root). That figure is *inferred*: it is to be confirmed against a PCSX2 card in
bring-up step B5.
