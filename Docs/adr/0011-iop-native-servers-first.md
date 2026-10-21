# ADR-0011: IOP as native RPC servers first; interpreting the game's IRX modules is deferred

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 by the project owner (M3 planning) |
| **Related** | architecture.md §5.2, §7.1, §8, Docs/milestones/M3-boots.md, ADR-0003 |

## Context

Architecture.md §5.2 planned three stages for the IOP: (A) run the disc's IRX modules on an IOP interpreter, (B)
replace the standard Sony modules with native services, (C) replace the game's own sound modules. M3's evidence
changes the order:

- Every SDK library on the EE side (libsif RPC, libcdvd, libpad, libmc, sceSdRemote) is recompiled game code; only
  kernel calls reach the host. Whatever answers must therefore speak the SIF command and RPC protocol, whichever
  stage is used.
- The game loads 15 modules from `MODULES/` after resetting the IOP with its update image, and binds 13 servers.
  Upstream PS2Recomp's IOP interpreter (`ps2xIOP`) has no SIO2, SPU2 or CDVD hardware and no SIF command decoding,
  so stage A would also need IOP-side SIF and RPC HLE, IOP threads in virtual time and an SPU2 register model.
- M3's goal (the title screen) needs disc access, module loading, the pad and the memory-card check, not audio.

## Decision

- M3 implements the IOP as **native services on the machine thread** (stage B first): the SIF link and protocol,
  and native servers for file I/O, the IOP heap, module loading, CD/DVD, the pad (old libpad protocol) and a minimal
  memory card.
- The game's own sound servers (`sceSdRemote` on SDRDRV, PCMPLAY) are answered by **null servers** that acknowledge
  every call with zeros and log it, until audio work starts (M4). Canned replies per (server, function) may be added
  from evidence when the game checks a value.
- Which native services stand in for which module is **game data** in the region profile (`[iop]`), not runtime code.
  A module the profile does not list stops the guest.
- Stage A (interpreting the sound IRX on an IOP interpreter) is reconsidered in M4; stage C may supersede it.

## Consequences

- No IOP code executes; IOP RAM only holds what the EE sends and what services allocate.
- Sound replies the game depends on must be understood (evidence from the EE-side wrappers and PCSX2 state) rather
  than produced by the original modules; the bring-up records each one.
- Native pad, memory-card and disc services exist from M3 on, which the launcher features need anyway.
