// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless preScript: set the auto-analysis options for the DQ8 boot ELF before analysis runs.
//
//   -preScript Dq8AnalysisOptions.java [on|off] [options-dump.txt]
//
// Turns "Decompiler Parameter ID" off (slow, not needed for a function map), turns the Emotion
// Engine extension's "MIPS-R5900 Constant Reference Analyzer" on when it is registered, and sets
// "Aggressive Instruction Finder" from the first argument (default off). The optional second
// argument receives a dump of every analysis option name and value after the changes: Ghidra's
// own names only, nothing from the program.
// @category DQ8

import java.io.File;
import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;

public class Dq8AnalysisOptions extends GhidraScript {

    private static final String PARAMETER_ID = "Decompiler Parameter ID";
    private static final String R5900_CONSTANTS = "MIPS-R5900 Constant Reference Analyzer";
    private static final String AGGRESSIVE = "Aggressive Instruction Finder";

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        boolean aggressive = args.length > 0 && args[0].equalsIgnoreCase("on");
        if (args.length > 0 && !args[0].equalsIgnoreCase("on") && !args[0].equalsIgnoreCase("off")) {
            throw new IllegalArgumentException("first argument must be 'on' or 'off', got: " + args[0]);
        }

        Map<String, String> current = getCurrentAnalysisOptionsAndValues(currentProgram);
        println("Dq8AnalysisOptions: " + current.size() + " analysis options registered for " +
                currentProgram.getLanguageID());

        set(current, PARAMETER_ID, false);
        set(current, R5900_CONSTANTS, true);
        set(current, AGGRESSIVE, aggressive);

        if (args.length > 1) {
            Map<String, String> after = new TreeMap<>(getCurrentAnalysisOptionsAndValues(currentProgram));
            File dump = new File(args[1]);
            try (PrintWriter out = new PrintWriter(dump)) {
                out.println("# Ghidra analysis options for " + currentProgram.getName() + " (" +
                        currentProgram.getLanguageID() + "), after Dq8AnalysisOptions");
                for (Map.Entry<String, String> e : after.entrySet()) {
                    out.println(e.getKey() + " = " + e.getValue());
                }
            }
            println("Dq8AnalysisOptions: wrote " + after.size() + " options to " + dump.getAbsolutePath());
        }
    }

    private void set(Map<String, String> current, String name, boolean enabled) {
        if (!current.containsKey(name)) {
            println("Dq8AnalysisOptions: option not registered, left alone: " + name);
            return;
        }
        String value = Boolean.toString(enabled);
        setAnalysisOption(currentProgram, name, value);
        println("Dq8AnalysisOptions: " + name + " = " + value + " (was " + current.get(name) + ")");
    }
}
