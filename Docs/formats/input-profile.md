# Input profile

An input profile is our own format: which keys and gamepad controls drive which control of the virtual
DualShock 2 (architecture.md §8.1, §9.3). It holds nothing from the disc. `dq8-game` reads one at start-up
(`--input-profile`, setting `input.profile`); the launcher's Controllers page reads and writes them (M5 session B).
Both go through one parser and writer, `dq8::core::input` (`include/dq8/core/input_profile.h`,
`src/core/input/input_profile.cpp`), whose tests are in `tests/unit/test_display.cpp`.

## Where

```
<config>/input/<name>.toml
```

- `<config>` is `user_config_dir()`: `$XDG_CONFIG_HOME/dq8-native` (default `~/.config/dq8-native`) on Linux, the
  user data directory on Windows and macOS.
- `<name>` is the setting `input.profile` (default `default`): 1 to 64 of `A-Z`, `a-z`, `0-9`, `_` and `-`.
  `dq8-game --input-profile` also takes a file: a value ending in `.toml` or holding a directory separator.
- No file for `default` means the built-in bindings below. No file for another name, or a file that does not parse,
  also gives the built-in bindings, with a warning on stderr: input never stops the game from starting.
- Profiles are separate files so that they can be shared and backed up on their own (architecture.md §9.3).

## Example

```toml
schema_version = 1

[keyboard]
cross = ["K", "Space"]
circle = ["L"]
start = ["Return"]

[gamepad]
cross = ["a"]
l2 = ["lefttrigger"]
left_stick_left = ["leftx-"]
left_stick_right = ["leftx+"]

[devices."030000005e040000120b000011050000"]
name = "Xbox Series X Controller"
port = 1
cross = ["b"]
circle = ["a"]
```

## Keys

| Key | Meaning |
|---|---|
| `schema_version` | Required, `1`. A later version is refused with a message naming the file. |
| `[keyboard]` | Bindings of the keyboard, which drives pad port 1. |
| `[gamepad]` | Bindings of every gamepad that has no `[devices]` entry of its own. |
| `[devices."<guid>"]` | One particular kind of gamepad, by its SDL GUID (`SDL_GUIDToString()`: 32 lowercase hex digits). Optional `name` (a string for people; never used to match), optional `port` (`1` or `2`: the pad port the device always takes when it is free), then any controls: those it lists replace `[gamepad]`'s for this device, the others are `[gamepad]`'s. |

No other key is allowed at the top level; an unknown key, control or source type is an error naming the file and
line (`settings-invalid`; a file that is not TOML is `settings-parse`).

**Controls** (the keys inside each table) are the DualShock 2's: `up`, `down`, `left`, `right`, `triangle`,
`circle`, `cross`, `square`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `start`, `select`, and the stick directions
`left_stick_up`, `left_stick_down`, `left_stick_left`, `left_stick_right`, `right_stick_up`, `right_stick_down`,
`right_stick_left`, `right_stick_right`.

**Values** are arrays of source names; any source in the list drives the control. An empty array unbinds the control.
A table that does not list a control keeps the built-in binding (so a file can hold only what it changes); a missing
`[keyboard]` or `[gamepad]` table is the built-in table. The writer (`format_profile`) always writes every control of
both tables, so a saved file is complete.

## Source names

Sources are SDL3's own names, so that `dq8-game` and the launcher turn them into SDL codes with SDL itself.

| Device | Names | From SDL |
|---|---|---|
| Keyboard | Key names: `K`, `Return`, `Space`, `Left`, `Left Shift`, `Keypad 8`, ... (case does not matter) | `SDL_GetScancodeName()` / `SDL_GetScancodeFromName()` |
| Gamepad button | `a`, `b`, `x`, `y` (the south, east, west and north face buttons **by position**, whatever their labels), `back`, `guide`, `start`, `leftstick`, `rightstick`, `leftshoulder`, `rightshoulder`, `dpup`, `dpdown`, `dpleft`, `dpright`, `misc1`, `paddle1` to `paddle4`, `touchpad` | `SDL_GetGamepadStringForButton()` / `SDL_GetGamepadButtonFromString()` |
| Gamepad axis | `leftx`, `lefty`, `rightx`, `righty`, `lefttrigger`, `righttrigger`, with a trailing `+` or `-` for one half (`leftx-` is the left stick pushed left, `lefty-` pushed up); no sign is the positive half, as for the triggers | `SDL_GetGamepadStringForAxis()` / `SDL_GetGamepadAxisFromString()` |

A name SDL does not know is skipped with a warning on stderr; the rest of the profile applies.

## How sources drive controls

- Every source gives an amount from 0 to 1: a key or button 0 or 1, a half axis its deflection to that side.
- A button control (`cross`, `l2`, `up`, ...) is pressed when one of its sources passes half of its range, or a
  quarter for `lefttrigger` and `righttrigger` (M3's trigger threshold). The game sees pressure 255 or 0.
- A stick axis is the largest amount among its positive direction's sources minus the largest among its negative
  direction's, so keys give full deflection and a stick its own amount.
- The gamepad's two sticks pass a radial dead zone first: setting `input.deadzone` (percent, default 8,
  `--deadzone`), rescaled so that the stick still reaches full deflection. Triggers have none.
- The keyboard drives pad port 1 together with the gamepad on that port; a gamepad stick that is
  deflected replaces the keyboard's stick keys, a centred one leaves them alone.

## Ports and hot-plug

`dq8-game` gives each gamepad a pad port when it arrives (`src/runtime/platform/sdl/sdl_ports.h`): the port its
GUID is pinned to (`port`) when free; else a free port it held before in this run; else a free port nobody held
yet; else any free port. A device is known by its GUID plus its serial number when SDL reports one, so a pad
unplugged and plugged in again gets its port back even when another pad arrived meanwhile, and two pads of one model
are told apart when they report serials. With both ports taken a third pad waits. The game sees port 2 as a pad
plugged in or out as its gamepad comes and goes.

## Built-in bindings

`default_profile()`, M3's fixed table:

| Control | Keyboard | Gamepad |
|---|---|---|
| D-pad | `Up`, `Down`, `Left`, `Right` | `dpup`, `dpdown`, `dpleft`, `dpright` |
| Triangle, Circle, Cross, Square | `I`, `L`, `K`, `J` | `y`, `b`, `a`, `x` |
| L1, R1 | `Q`, `E` | `leftshoulder`, `rightshoulder` |
| L2, R2 | `1`, `3` | `lefttrigger`, `righttrigger` |
| L3, R3 | `2`, `4` | `leftstick`, `rightstick` |
| Start, Select | `Return`, `Backspace` | `start`, `back` |
| Left stick up, down, left, right | `W`, `S`, `A`, `D` | `lefty-`, `lefty+`, `leftx-`, `leftx+` |
| Right stick up, down, left, right | `T`, `G`, `F`, `H` | `righty-`, `righty+`, `rightx-`, `rightx+` |

`Alt+Enter` is the window's (it toggles full screen) and never reaches the pad, even with `Return` bound.
