# ADR-0013: Guarded guest-state writes from the game layer

| | |
|---|---|
| **Status** | **Proposed** 2026-10-21, for the project owner. Implemented behind `--movies skip` (the default) for one row, TITLE's flyover; `--movies blank` turns it off |
| **Related** | architecture.md §4.1, §4.5, §7.2, §10.2, §12; ADR-0012; Docs/milestones/M3-boots.md (bring-up log, "Black screen after the skipped movies", "Flyover timeline skip") |

## Context

M3 skips movies by overriding the movie player API: open and start return at once, and the "finished?" query reports
the movie done. The TITLE scene does not wait for its flyover movie, though. Stage 10 steps its own frame counter,
the word at `0x004670C0`, once per 30 Hz frame, whether or not the movie plays. The title fades in from step 638 and is
complete at 648. With the movie skipped, a player who presses nothing sees about 21 s of black (the title marker came
at vblank 2640 instead of about 1,370). On the PS2 a button press during the flyover makes the game write 648 to that
counter itself (TITLE `0x00464500`).

No override fits: nothing in the flyover returns early or reports done. The stage update is one function, and the
press check sits in the middle of it. Two other routes fail:

- A byte patch on code does nothing, because the code that runs is native.
- A load-time data patch is undone, because the stage's init (`0x00463DD0`) zeroes the counter.

What is left is writing guest state while the game runs. Architecture.md §10.2 plans patches as declarative data
applied "on demand", but nothing writes guest memory from outside guest code yet. Overrides write registers and
memory, but only as the function they replace. So this needs a rule.

## Proposal

A **guarded guest-state write** is a write to a guest variable by the game layer (`src/game`), outside any guest
function. It follows these rules:

1. **Data, not code.** The row comes from the region profile (`[[movie.timelines]]`): which overlay must be resident,
   a stage variable and its value, the variable to write, and our value. The profile carries addresses and numbers
   only, never original bytes (§14). The code in `src/game` is generic; a second region is a new profile.
2. **Guarded.** Every condition is read from guest memory at the moment of the write:
   - the overlay id in the window header;
   - the stage variable;
   - the variable's own value, `0 < counter < skip_to`: the scene has stepped it, and it has not reached the skip
     point. The stage value alone is not enough, because SLUS-21207's stage halfword holds 10 from power-on (its initial
     value in the program's data) until the title sequence first sets it.

   The device keeps no state, so whether it writes is a function of guest memory at that vblank, which keeps snapshots
   and replay simple (§4.1). It writes only data or bss: the profile loader rejects a variable inside an overlay's
   text.
3. **The game's own value.** We write only a value the game itself writes in the same situation, here the step a
   button press jumps to. The bring-up log records the evidence. The scene then goes on through its own code: the
   fade, the menu, the attract sequence after 4,110 idle steps.
4. **At a safe point, in virtual time.** The write happens in a vblank-start listener (phase `intc`, after the GS,
   kernel and IOP phases) on the machine thread. That listener runs inside a machine event, at a guest safe point (§12),
   so the write is deterministic: two runs give identical event logs.
5. **Visible.** Every write emits an event (`movie_timeline_skip` {timeline, from, to}), so event logs and determinism
   checks show it.
6. **Opt-in by policy.** A host or feature setting enables it: `--movies skip` (the default) enables it, and
   `--movies blank` keeps M3's behaviour (the scene's clock runs on black, which keeps timing parity with a console
   run that plays the movie).

## Consequences

- With `--movies skip` and no press after the first-run settings, the title arrives at vblank 1368 instead of 2640.
  The screen is black only for the cut after the publisher logo (about vblanks 1138 to 1152).
- An input script written for the black flyover has one press too many: the press that ended the flyover now opens
  the title menu. `tests/local/input/boot-to-title.dq8in` (M3a acceptance 1) therefore runs with `--movies blank`, and
  meets at vblank 1200 with M3a's GS and RAM hashes. Under `skip` it meets at 1080. The settings-only script
  `boot-to-title-settings-only.dq8in` covers `skip`.
- This is the first runtime patch in the sense of §10.2. When the patch system lands, a timeline row should become one
  kind of patch: "conditional write at vblank", keyed by (profile, unit, address). It would carry our value and the
  same guards, and the patch engine would apply it. Until then the rows live under `[movie]`, because movies are their
  only user.
- Not covered: writes that must happen inside a guest frame, writes to code, and writes whose value is not one the
  game itself produces. Those still need an override or a decision of their own.

## Alternatives considered

- **Inject a button press** through the pad service. This writes no guest memory, but the press has to last exactly
  one game frame: a press still held after step 648 opens the menu. It would also mix synthetic input into a script
  or the player's own input.
- **A `hook_before` override on the stage update** (TITLE `0x00464500`). The hook kind exists (`--stop-at-pc` uses
  it), and the override would be keyed like any other. But it still writes guest state from native code, so it needs
  the same rule. It also ties the write to one function's boundaries in the TITLE map and to that overlay's digest,
  where the vblank device needs only addresses.
- **Leave the black** (M3's choice). This keeps 21 s of black after every skipped movie in normal play.
