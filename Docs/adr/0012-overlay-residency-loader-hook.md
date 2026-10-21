# ADR-0012: Overlay residency through the loader hook

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 with the M3 plan; to be confirmed by M3 bring-up |
| **Related** | architecture.md §4.3, §4.5, spike S3, Docs/milestones/M3-boots.md |

## Context

Architecture.md §4.3 named three ways to learn which overlay is resident: replace the loader `0x00100330` with a
native implementation (primary), hook the empty function `0x00100320(dest)` the loader calls after the read and
before the cache flush (cheaper variant), or read the overlay id at `window + 4` (fallback). M3 implements the file-I/O
RPC natively anyway, because the game reads its `.HD6` archive indexes through it, so the game's own loader can load
the bytes with no extra native code.

## Decision

- The game's own loader runs unchanged over the native file-I/O service and places the overlay bytes in guest RAM.
- An override on `loader_hook` (`0x00100320` on SLUS-21207, from the region profile) parses the container header at
  the window, cross-checks the overlay id at `window + 4` and the header's sizes against the profile, and makes that
  overlay resident in the `OverlayManager` before the loader flushes caches and runs the constructor table. A mismatch
  stops the guest.
- A native loader replacement stays a possible later optimisation (faster loads); the header check at `window + 4`
  stays as the debug-build safety net on every dispatch into the window.

## Consequences

- One file-I/O implementation serves archives and overlays; the overlay path exercises it on every switch.
- The override fires exactly between "bytes in RAM" and "code live", the point where residency must change.
- The decision rests on S3's finding of a single `jal` call site for the hook; bring-up confirms it on the real boot.
