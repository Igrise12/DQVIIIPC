# ADR-0009: License the project under GPL-3.0-or-later

| | |
|---|---|
| **Status** | Accepted |
| **Date** | 2026-10-19 |
| **Decided by** | Project owner |
| **Related** | project-brief.md §9 and §14 (D2), tech-stack.md §10 |

## Context

The project is an open-source native port built on PS2Recomp, which is licensed GPL-3.0. Our runtime builds on the
conventions of PS2Recomp's generated code and may reuse its runtime code, so the combined work must be distributable
under the GPL-3.0.
The first GS backend, paraLLEl-GS, is LGPL-3.0-or-later and is compatible with a GPL-3.0 project. Sibling PS2 ports
use GPL-3.0 (god-of-war-recomp) or MIT (ico-recomp, rac1-native-game, where no GPL code is linked).

## Decision

All first-party code is licensed **GPL-3.0-or-later**.

- The full license text is in `LICENSE` at the repository root.
- Every first-party source file carries `SPDX-License-Identifier: GPL-3.0-or-later`. CI checks this with REUSE once CI exists.
- Dependencies must be GPL-3.0-compatible, per the policy in tech-stack.md §10. GPL-2.0-only code is not allowed.
- LGPL libraries (paraLLEl-GS, FFmpeg if used) are linked dynamically so users can substitute them.
- Contributions are accepted under the same license with a Developer Certificate of Origin sign-off. There is no contributor license agreement.

## Consequences

- We can link and adapt PS2Recomp and GPL-3.0 code from PCSX2, with attribution.
- A permissive license is not available unless we later replace the recompiler and runtime with clean-room code.
- Downstream distributors must provide source for the host. The locally built game module is generated on the user's machine and is never distributed (architecture.md §2).
- Original art, icons and fonts must be our own or under a compatible license. No Square Enix assets (tech-stack.md §10).

## Not covered

This ADR records a licensing choice. It is not legal advice, and the brief (§9) still calls for a legal review before the first public release.
