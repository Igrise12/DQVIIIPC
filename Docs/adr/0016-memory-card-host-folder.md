# ADR-0016: The memory card is a host folder behind the native mcserv, with a virtual clock

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 by the project owner (M4 planning) |
| **Related** | architecture.md §8.2; ADR-0011; Docs/milestones/M4-playable.md; Docs/formats/memory-card-folder.md (M4 session D) |

## Context

M3's memory-card server answers a fixed "card present, unformatted" and stops on Format. Saves are part of M4's exit.
Architecture §8.2 planned a folder per card with atomic writes and backups; importing standard PS2 card images is a P1
feature. The owner chose on 2026-10-21 to support the host folder only in M4.

## Decision

- Each memory card is a **folder on the host** (`<save-dir>/<profile>/card1/` for port 0), holding the game's
  directories and files as plain files plus small metadata files of our own (format state, entry order, timestamps).
- The native mcserv translates the game's memory-card RPCs onto an in-memory card model, which alone decides what the
  guest sees; the folder is loaded at start and written behind on the I/O thread with atomic replaces and rolling
  backups. Disk speed and errors never reach the guest.
- Timestamps come from a **virtual real-time clock** (a fixed date by default, or the host's time recorded in the run's
  start event), never from the host clock directly.
- Headless runs default to an in-memory card, so scripted runs never touch user files; the window defaults to the
  platform's user data directory.

## Consequences

- Saves stay plain files on the user's machine and are easy to back up.
- Runs stay deterministic: the same folder content and input give the same guest-visible results.
- `.ps2` card image import remains a later P1 feature on top of the same card model.
