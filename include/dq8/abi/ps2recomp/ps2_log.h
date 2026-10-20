// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Generated code includes ps2_log.h under #ifdef _DEBUG (stubs) or PS2_FUNCTION_LOG_TRACKER (functions) and
// then writes PS_LOG_ENTRY("<function name>"); at the top of the body. Upstream logs every call to a file; we
// keep the hook as a no-op, since call tracing belongs to the host (dq8-game --trace-dispatch).
#pragma once

#define PS_LOG_ENTRY(name) static_cast<void>(0)
