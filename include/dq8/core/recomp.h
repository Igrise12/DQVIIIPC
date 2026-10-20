// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

// The recompilation stages of the setup pipeline (architecture.md §3, stages 3 to 6): code units, function
// maps, recompiler configs, the recompiler driver, the module build and their orchestration over a work
// directory. Namespace dq8::core::recomp.
#include "dq8/core/recomp/function_map.h"
#include "dq8/core/recomp/module_build.h"
#include "dq8/core/recomp/pipeline.h"
#include "dq8/core/recomp/recomp_config.h"
#include "dq8/core/recomp/recomp_driver.h"
#include "dq8/core/recomp/units.h"
