# ADR-0014: The game's sound modules run on an IOP interpreter, with a register-level SPU2

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 by the project owner (M4 planning). Amends ADR-0011 for the sound modules |
| **Related** | architecture.md §5.2, §7.1, §12.2; ADR-0005, ADR-0007, ADR-0011; Docs/milestones/M4-playable.md |

## Context

ADR-0011 answered the game's two sound servers (`sceSdRemote` on SDRDRV, `0x80000701`, and PCMPLAY, `0x123456`) with
null servers and deferred the question of how sound is produced to M4. Three ways were open: run the disc's sound
modules on an IOP interpreter (architecture §5.2 stage A), re-implement their protocols natively (stage C), or stay
silent.

What M4 planning established (Docs/milestones/M4-playable.md, "What exploration established"):

- The ten sound modules (LIBSD, SDRDRV, MODHSYN, MODMIDI, MODMSIN, SKSOUND, SKHSYNTH, SKMIDI, SKMSIN, PCMPLAY) import
  66 IOP kernel functions from eleven libraries (intrman, loadcore, sysmem, sysclib, stdio, thbase, thsemap, thevent,
  timrman, sifman, sifcmd), plus each other's exports. Their code is user-mode MIPS I integer code only.
- Only LIBSD touches hardware: the SPU2 registers, IOP DMA channels 4 and 7, and the DMA control registers.
- The `SK*` protocol is undocumented (architecture §19 Q8). SDRDRV is also an IOP-to-EE RPC client; PCMPLAY feeds
  movie audio through the SPU2's AutoDMA.

Re-implementing the protocols would mean reverse-engineering the sequencer, synthesiser and stream modules first;
interpreting them needs only the small kernel surface above and a faithful SPU2.

## Decision

- The sound modules run unchanged on an **IOP interpreter** (R3000A, user mode) inside the runtime, on the machine
  thread in virtual time (ADR-0007): IOP threads are register contexts scheduled by an HLE IOP kernel, IOP code runs
  only from its own slice events, and one IOP cycle is exactly eight EE cycles.
- The IOP kernel libraries they import are **HLE exports**; an import nobody implemented stops the guest with the
  library and ordinal named. Modules link to each other through their own export tables, as the real loader does.
- Which modules run interpreted is region-profile data: `services = ["interpret"]` in `[iop]`. The native servers of
  ADR-0011 stay for file I/O, the IOP heap, module loading, CD/DVD, the pad and the memory card.
- SIF RPC routes a server id to a native server or to an interpreted module's server; the IOP can also call EE-side
  servers (SDRDRV's callbacks).
- A **register-level SPU2** (two cores of 24 voices, ADPCM, envelopes, reverb, manual and AutoDMA, IRQ) is mixed on
  the machine thread in virtual time; behaviour may be adapted from PCSX2 (GPL-3.0-or-later, with attribution).
  Output goes through a lock-free ring to an SDL3 audio stream on its own audio thread (ADR-0005); nothing the guest
  observes depends on the host's audio timing, and fast-forward drops audio.

## Consequences

- No reverse engineering of the sound protocol is needed before sound works; Q8 is answered by running the modules.
- Session A (interpreter, kernel HLE, loader, RPC routing) and session B (SPU2, mixer, audio output) of M4 build it;
  they meet at the `iop/hw/iop_hw.h` seam.
- The IOP interpreter and the SPU2 cost CPU time on the machine thread; they are measured, and stage C (native or
  ahead-of-time sound modules) stays a later optimisation.
- With no interpreted module the runtime schedules no IOP or SPU2 event, so the M3 path is unchanged.
