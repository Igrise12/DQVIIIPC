# `.dq8in`: scripted pad input

A `.dq8in` file is our own format: what the two DualShock 2 ports hold, vblank by vblank, for a scripted or
recorded run of `dq8-game` (`--input-script FILE`, written by `--record-input FILE`). It holds only input we
authored or recorded, never game data. The runtime reads it with `rt::ScriptInput`
(`src/runtime/io/input_script.h`). Its tests are in `tests/unit/test_input_script.cpp`.

## Time

The unit is the **vblank**: vblank 0 is the first vblank start after boot. The IOP pad service samples both ports
once per vblank start, through `IInputSource::sample(port, vblank)`. A line "at vblank N" decides what the game
reads in the frame that starts at vblank N.

Every line says when it takes effect, in one of three ways:

| Form | Takes effect at |
|---|---|
| `N ...` | vblank `N`, but never before the previous line |
| `+N ...` | `N` vblanks after the previous line (after vblank 0 for the first line) |
| `after TRIGGER +N ...` | `N` vblanks after the first occurrence of `TRIGGER` at or after the previous line |

Lines take effect in file order. Once a line waits for a trigger, the lines after it wait too. A trigger that
fired before the previous line's time does not count. This keeps a script a sequence: "the settings screen,
then 30 vblanks later Cross" stays true even when a mode repeats.

A trigger happens when the machine emits the matching event, and its time is the vblank count at that moment.
If a line's time has already passed when its trigger arrives, the line takes effect at the next sample, for its
full length.

| Trigger | Event | Value |
|---|---|---|
| `overlay:NAME` | `overlay_resident`: the residency hook made overlay `NAME` resident (ADR-0012) | the profile's overlay name, any case: `overlay:TITLE` |
| `mode:N` | `mode`: the game-mode variable became `N` | decimal or `0x` hex |
| `rpc_bind:SID` | `rpc_bind`: the game bound IOP RPC server `SID` | decimal or `0x` hex: `rpc_bind:0x80000100` |

## Syntax

```
dq8in 1                               # header: the first non-comment line, format version 1
# comment; '#' starts a comment anywhere on a line

600 press cross                       # vblank 600: Cross for 4 vblanks (the default length)
+30 press cross 6                     # 30 vblanks later: Cross for 6 vblanks
after overlay:TITLE +120 press down   # 120 vblanks after TITLE became resident
+20 p2 hold start                     # port 2 from here on
after mode:4 +0 release all           # the first mode-4 entry after the line before
```

A line is `TIME [p1|p2] ACTION`. `p1` (the default) is port 0 and `p2` is port 1.

| Action | Effect on the port |
|---|---|
| `press BUTTONS [N]` | pressed for `N` vblanks (default 4, at least 1), on top of what is held |
| `hold BUTTONS` | pressed until released |
| `release BUTTONS` | stops holding them; `release all` also ends running presses |
| `state BUTTONS [lx=V] [ly=V] [rx=V] [ry=V]` | exactly this: holds = `BUTTONS` (or `none`), presses ended, sticks as given, others centred |
| `stick left\|right X Y` | one stick's axes, 0..255 (0x80 is the centre); kept until changed |
| `disconnect`, `connect` | unplug and replug: a disconnected port reads no buttons, and its holds and sticks are kept for the reconnect |

`BUTTONS` is one button or several joined by `+` (`down+cross`): `select`, `l3`, `r3`, `start`, `up`, `right`,
`down`, `left`, `l2`, `r2`, `l1`, `r1`, `triangle`, `circle`, `cross`, `square`. Names are case-insensitive.
Pressed buttons read full pressure (255); others read 0.

Numbers are decimal, or hex with `0x`. A malformed line stops the load with `file:line: problem`.

## Recordings

`--record-input FILE` writes the header, a comment, and one line per change of a port's state at its absolute
vblank: `N [p2] state BUTTONS [axes]`, `N [p2] disconnect`, `N [p2] connect`. A reconnect is followed by a full
`state` line. Replaying the file with `--input-script` gives the same buttons, sticks and connection on every
vblank. Pressure is not recorded, because scripts derive it from the buttons.
