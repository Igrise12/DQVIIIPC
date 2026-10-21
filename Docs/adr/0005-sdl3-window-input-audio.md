# ADR-0005: SDL3 for window, input and audio

| | |
|---|---|
| **Status** | **Accepted** 2026-10-21 by the project owner, at the start of milestone M3 |
| **Related** | architecture.md §8.1, §11, §12.2, tech-stack.md §5, Docs/milestones/M3-boots.md |

## Context

M3 puts the first pixels in a window and needs live keyboard and gamepad input; M4 adds audio; M5 builds the
launcher. One cross-platform library can cover the window, input, gamepads (with a mapping database and haptics),
audio output and native file dialogs on Linux, Windows and later macOS. SDL3 does all of these under the Zlib
license, is already used by sibling projects on this machine, and is what paraLLEl-GS's own tools use. SDL3 3.4.16
is installed system-wide on the development machine; Ubuntu 24.04 (the CI image) has no SDL3 package.

## Decision

- SDL3 is the platform layer for the window, input, gamepads and audio output of `dq8-game`, and for the launcher.
- It is a git submodule (`third_party/SDL`, pinned to `release-3.4.18`, the latest stable release on 2026-10-21) built
  with the project; the option `DQ8_USE_SYSTEM_SDL3` (default OFF) uses an installed SDL3 instead for faster local
  builds.
- SDL3 code lives behind `IPlatform` (`src/runtime/platform/platform.h`) in its own library (`dq8_platform_sdl`),
  linked only by the host. The machine thread never calls SDL: input reaches the guest through a per-vblank latch so
  that scripted and recorded runs stay deterministic (ADR-0007).

## Consequences

- One dependency instead of separate window, input and audio libraries; the gamepad database comes with it.
- CI builds SDL3 from source on Linux and Windows (a few minutes, cached later); window tests use SDL's dummy video
  driver.
- Presentation in M3 is a CPU copy of the scanout into an SDL texture; the Vulkan presentation pipeline of
  architecture.md §6.3 replaces it later without changing `IPlatform`'s callers.
