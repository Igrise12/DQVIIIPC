// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// The contract unit's one export, as the module template's dq8_unit_entry.cpp.in provides it. Built into the
// dq8_unit_contract shared library that the dq8-game --elf test loads.
#include "support/contract_unit.h"

extern "C" DQ8_MODULE_EXPORT const dq8_game_module_desc* dq8_game_module_describe(void) {
    return contract_unit_descriptor();
}
