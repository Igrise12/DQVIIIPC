// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless postScript: export the function map of one code unit for ps2_recomp.
//
//   -postScript ExportFunctionMap.java <out.csv> <out.jump_tables.toml> <out.stats.toml> <unit_start> <unit_end>
//               [entry_tables=START:END[,START:END...]]
//
// entry_tables: pointer tables whose words are function entries (the static-constructor table crt0 walks,
// from the region profile's [boot].ctor_table). A word that points into the unit at an address Ghidra holds no
// function for is disassembled and made a function before the export: such code is reached only through the
// table, so auto-analysis never found it (M3 bring-up: 6 of main's 156 constructors). The project is opened
// read-only, so these functions exist only for the export.
//
// The unit is the half-open address range [unit_start, unit_end): the main program's file image, or
// one overlay's 64-byte header plus text plus data at the window base. Three files are written:
//
// - <out.csv>: "Name,Start,End,Size" (addresses 0x%08X, End exclusive, Size decimal), one row per
//   function, sorted, never overlapping:
//   1. Functions: every Ghidra function whose entry lies in the unit, is 4-aligned and holds an
//      instruction. Labels (LAB_, caseD_, switchD_) are never exported; a function whose Ghidra
//      name is such a label is written as FUN_<address>. End = min(body max + 1, next entry,
//      unit_end); rows shorter than 4 bytes are dropped.
//   2. Case blocks: a function whose only references are one switch's jump, words of its table, and
//      non-call flow from the switch owner or its other case blocks is a case block, not a
//      function. It joins the owner's body (and stops being a row) when it directly follows the
//      owner. LoadOverlay.java creates such blocks: it seeds a function at every pointer in an
//      overlay's data, jump tables included.
//   3. Coverage: a flow target of exported code (branch, jump, case, fall-through) inside the unit
//      that no range covers extends the range before it over the contiguous run of Ghidra
//      instructions that reaches the target (never past the next entry, never over words Ghidra did
//      not disassemble). A target in an island of instructions that cannot be reached so becomes a
//      row of its own, sub_<address>, spanning the island. What is still uncovered is a hole.
// - <out.jump_tables.toml>: one [[jump_tables.table]] per computed jr (rs != $ra) whose targets
//   Ghidra resolved and whose table could be located: keyed by the table's data address, computed
//   from the lw that loads the jump register (lui/addiu/ori constant plus the lw offset), with
//   base_register = that lw's base register and the entries in table order, read from memory,
//   checked against Ghidra's targets and cut at the slti/sltiu case count when there is one.
// - <out.stats.toml>: link_return_entries (bgezal/bltzal/bal and their likely forms whose target is
//   outside their function, plus 8: the recompiler emits no resume entry there; these become
//   entry_points), then lists (table sites, unresolved computed-jump sites, cut bodies, multi-range
//   bodies, case blocks joined, ranges extended, islands, holes, tables whose key ps2_recomp's own
//   matcher would not compute) and a [counts] table.
//
// Instruction words are decoded from memory here (MIPS field layout), not from Ghidra's
// mnemonics, so the scan sees exactly what the recompiler will decode in each [Start, End).
// Output holds addresses, sizes, counts and Ghidra's names; no bytes. Read-only: run it with
// -noanalysis -readOnly. Contributor tool only; never on the end-user setup path.
// @category DQ8

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class ExportFunctionMap extends GhidraScript {

    // Major opcodes (bits 31..26) and function / rt sub-codes used by the scans.
    private static final int OP_SPECIAL = 0x00;
    private static final int OP_REGIMM = 0x01;
    private static final int OP_JAL = 0x03;
    private static final int OP_ADDI = 0x08;
    private static final int OP_ADDIU = 0x09;
    private static final int OP_SLTI = 0x0A;
    private static final int OP_SLTIU = 0x0B;
    private static final int OP_ORI = 0x0D;
    private static final int OP_LUI = 0x0F;
    private static final int OP_COP0 = 0x10;
    private static final int OP_COP1 = 0x11;
    private static final int OP_COP2 = 0x12;
    private static final int OP_DADDI = 0x18;
    private static final int OP_DADDIU = 0x19;
    private static final int OP_MMI = 0x1C;
    private static final int OP_LW = 0x23;
    private static final int OP_LWU = 0x27;
    private static final int FN_JR = 0x08;
    private static final int FN_JALR = 0x09;
    private static final int FN_ADDU = 0x21;
    private static final int FN_DADDU = 0x2D;
    private static final int RT_BLTZAL = 0x10;
    private static final int RT_BGEZAL = 0x11;
    private static final int RT_BLTZALL = 0x12;
    private static final int RT_BGEZALL = 0x13;
    private static final int REG_RA = 31;

    // Backward search windows, in instructions. The recompiler's own jump-table matcher looks 20
    // back for the lw, 10 back for the addu and 20 back for the lui.
    private static final int WRITER_LOOKBACK = 20;
    private static final int BOUND_LOOKBACK = 30;
    private static final int MAX_TABLE_ENTRIES = 4096;
    private static final long NO_VALUE = Long.MIN_VALUE;

    private static final class Fn {
        final String name;
        final long entry;
        final AddressSet body;
        long end;

        Fn(String name, long entry, AddressSetView body) {
            this.name = name;
            this.entry = entry;
            this.body = new AddressSet(body);
        }
    }

    // What one pass over the exported ranges found.
    private static final class Scan {
        final List<Table> tables = new ArrayList<>();
        final TreeSet<Long> linkReturns = new TreeSet<>();
        final TreeSet<Long> linkInternal = new TreeSet<>();
        long rangeWordsNotInstructions;
        long computedJr;
        long jalr;
    }

    private static final class Table {
        final long site;
        final long function;
        final long address;
        final int baseRegister;
        final List<Long> targets;
        final String bound;
        final long recompKey;

        Table(long site, long function, long address, int baseRegister, List<Long> targets, String bound,
                long recompKey) {
            this.site = site;
            this.function = function;
            this.address = address;
            this.baseRegister = baseRegister;
            this.targets = targets;
            this.bound = bound;
            this.recompKey = recompKey;
        }
    }

    private final List<String> unresolved = new ArrayList<>();
    private final List<String> overlaps = new ArrayList<>();
    private final List<String> multiRange = new ArrayList<>();
    private final List<String> keyMismatches = new ArrayList<>();
    private final List<String> nonInstructionWords = new ArrayList<>();
    private final List<String> tableSites = new ArrayList<>();
    private final List<String> absorbed = new ArrayList<>();
    private final List<String> extended = new ArrayList<>();
    private final List<String> holes = new ArrayList<>();
    private final List<String> islands = new ArrayList<>();
    private final Map<String, Long> counts = new LinkedHashMap<>();
    private final TreeSet<Long> tableEntryFunctions = new TreeSet<>();

    private Memory memory;
    private Listing listing;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 5 && args.length != 6) {
            throw new IllegalArgumentException("usage: ExportFunctionMap.java <out.csv> <out.jump_tables.toml> " +
                    "<out.stats.toml> <unit_start> <unit_end> [entry_tables=START:END[,START:END...]]");
        }
        File csvFile = new File(args[0]);
        File tablesFile = new File(args[1]);
        File statsFile = new File(args[2]);
        long unitStart = Long.decode(args[3]);
        long unitEnd = Long.decode(args[4]);
        if (unitStart < 0 || unitEnd <= unitStart || unitEnd > 0x1_0000_0000L) {
            throw new IllegalArgumentException("bad unit range " + args[3] + " " + args[4]);
        }
        memory = currentProgram.getMemory();
        listing = currentProgram.getListing();
        if (args.length == 6) {
            createTableEntryFunctions(args[5], unitStart, unitEnd);
        }

        // Pass 1 finds the switches; case blocks held as functions join their owner; pass 2 is final.
        List<Fn> functions = collectFunctions(unitStart, unitEnd);
        assignEnds(functions, unitEnd);
        Scan first = scan(functions);
        int absorbedCount = absorbCaseBlocks(functions, first.tables, unitStart, unitEnd);
        unresolved.clear();
        overlaps.clear();
        multiRange.clear();
        nonInstructionWords.clear();
        assignEnds(functions, unitEnd);
        long maxEnd = coverFlowTargets(functions, unitStart, unitEnd);
        Scan scan = scan(functions);
        List<Table> tables = scan.tables;
        TreeSet<Long> linkReturns = scan.linkReturns;
        TreeSet<Long> linkInternal = scan.linkInternal;

        // One table per data address; two sites sharing a table emit it once.
        Map<Long, Table> byAddress = new LinkedHashMap<>();
        long tableEntries = 0;
        long sharedTables = 0;
        long boundedBySlti = 0;
        long targetsOutside = 0;
        for (Table t : tables) {
            if (t.bound.equals("slti")) {
                boundedBySlti++;
            }
            tableSites.add(String.format("{ site = %s, function = %s, table = %s, entries = %d, bound = \"%s\" }",
                    hex(t.site), hex(t.function), hex(t.address), t.targets.size(), t.bound));
            if (t.recompKey != t.address) {
                keyMismatches.add(String.format("{ site = %s, table = %s, recomp_key = %s }", hex(t.site),
                        hex(t.address), t.recompKey == NO_VALUE ? "\"none\"" : hex(t.recompKey)));
            }
            Fn owner = null;
            for (Fn f : functions) {
                if (f.entry == t.function) {
                    owner = f;
                    break;
                }
            }
            for (long target : t.targets) {
                if (owner != null && (target < owner.entry || target >= owner.end)) {
                    targetsOutside++;
                }
            }
            Table prev = byAddress.putIfAbsent(t.address, t);
            if (prev != null) {
                sharedTables++;
                if (!prev.targets.equals(t.targets)) {
                    unresolved.add(String.format("{ site = %s, function = %s, reason = \"table-conflict\" }",
                            hex(t.site), hex(t.function)));
                }
            }
            else {
                tableEntries += t.targets.size();
            }
        }

        // Functions nobody inside the unit references (only main, or nothing, points at them).
        long unreferenced = 0;
        long outsideOnly = 0;
        ReferenceManager refs = currentProgram.getReferenceManager();
        for (Fn f : functions) {
            boolean inside = false;
            boolean outside = false;
            ReferenceIterator it = refs.getReferencesTo(toAddr(f.entry));
            while (it.hasNext()) {
                Reference r = it.next();
                long from = ramOffset(r.getFromAddress());
                if (from >= unitStart && from < unitEnd) {
                    inside = true;
                }
                else {
                    outside = true;
                }
            }
            if (!inside && !outside) {
                unreferenced++;
            }
            else if (!inside) {
                outsideOnly++;
            }
        }

        long rangeBytes = 0;
        long autoNamed = 0;
        long thunks = 0;
        long labelNamed = 0;
        for (Fn f : functions) {
            rangeBytes += f.end - f.entry;
            String name = csvName(f);
            if (!name.equals(f.name)) {
                labelNamed++;
            }
            if (name.startsWith("FUN_") || name.startsWith("thunk_FUN_") || name.startsWith("sub_")) {
                autoNamed++;
            }
        }
        FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(toAddr(unitStart), true);
        while (fi.hasNext()) {
            Function f = fi.next();
            if (f.getEntryPoint().getOffset() >= unitEnd) {
                break;
            }
            if (f.isThunk()) {
                thunks++;
            }
        }
        long unitInstructions = 0;
        long unitInstructionsOutside = 0;
        int fIdx = 0;
        for (Instruction ins : listing.getInstructions(new AddressSet(toAddr(unitStart), toAddr(unitEnd - 1)), true)) {
            long a = ins.getAddress().getOffset();
            unitInstructions++;
            while (fIdx < functions.size() && functions.get(fIdx).end <= a) {
                fIdx++;
            }
            if (fIdx >= functions.size() || a < functions.get(fIdx).entry) {
                unitInstructionsOutside++;
            }
        }

        put("functions", functions.size());
        put("case_blocks_absorbed", absorbedCount);
        put("ranges_extended_over_flow_targets", extended.size());
        put("islands_added", islands.size());
        put("holes_remaining", holes.size());
        put("functions_auto_named", autoNamed);
        put("functions_label_named_renamed", labelNamed);
        put("functions_thunk_in_unit", thunks);
        put("functions_unreferenced", unreferenced);
        put("functions_referenced_only_from_outside", outsideOnly);
        put("range_bytes", rangeBytes);
        put("range_words_not_ghidra_instructions", scan.rangeWordsNotInstructions);
        put("highest_end", maxEnd);
        put("functions_with_non_instruction_words", nonInstructionWords.size());
        put("unit_instructions", unitInstructions);
        put("unit_instructions_outside_ranges", unitInstructionsOutside);
        put("computed_jr_sites", scan.computedJr);
        put("jump_tables", byAddress.size());
        put("jump_table_sites", tables.size());
        put("jump_table_shared_by_sites", sharedTables);
        put("jump_table_entries", tableEntries);
        put("jump_tables_bounded_by_slti", boundedBySlti);
        put("jump_table_targets_outside_function", targetsOutside);
        put("jump_tables_recomp_key_mismatch", keyMismatches.size());
        put("unresolved_sites", unresolved.size());
        put("jalr_sites", scan.jalr);
        put("link_return_entries", linkReturns.size());
        put("link_branch_internal", linkInternal.size());
        put("overlaps", overlaps.size());
        put("multi_range_bodies", multiRange.size());

        writeCsv(csvFile, functions);
        writeTables(tablesFile, byAddress.values(), unitStart, unitEnd);
        writeStats(statsFile, unitStart, unitEnd, linkReturns, linkInternal);

        println(String.format("ExportFunctionMap: unit %s-%s: %d functions (%d case blocks joined, %d islands), " +
                "%d jump tables, %d unresolved computed-jump sites, %d link-return entries, %d overlaps, " +
                "%d holes; wrote %s", hex(unitStart), hex(unitEnd), functions.size(), absorbedCount, islands.size(),
                byAddress.size(), unresolved.size(), linkReturns.size(), overlaps.size(), holes.size(),
                csvFile.getAbsolutePath()));
    }

    // Every word of every exported range, as the recompiler will decode it: jump tables, link
    // branches, words Ghidra did not disassemble.
    private Scan scan(List<Fn> functions) throws Exception {
        Scan scan = new Scan();
        for (Fn f : functions) {
            monitor.checkCancelled();
            int n = (int) ((f.end - f.entry) / 4);
            int[] words = new int[n];
            int notInstructions = 0;
            for (int i = 0; i < n; i++) {
                long a = f.entry + 4L * i;
                words[i] = readWord(a);
                if (listing.getInstructionAt(toAddr(a)) == null) {
                    notInstructions++;
                }
            }
            if (notInstructions > 0) {
                scan.rangeWordsNotInstructions += notInstructions;
                nonInstructionWords.add(String.format("{ function = %s, words = %d }", hex(f.entry), notInstructions));
            }
            for (int i = 0; i < n; i++) {
                int w = words[i];
                long a = f.entry + 4L * i;
                int op = op(w);
                if (op == OP_SPECIAL && fn(w) == FN_JR && rs(w) != REG_RA) {
                    scan.computedJr++;
                    Table t = jumpTable(f, words, i);
                    if (t != null) {
                        scan.tables.add(t);
                    }
                }
                else if (op == OP_SPECIAL && fn(w) == FN_JALR) {
                    scan.jalr++;
                }
                else if (op == OP_REGIMM && isLinkBranch(rt(w))) {
                    long target = (a + 4 + ((long) simm(w) << 2)) & 0xFFFF_FFFFL;
                    if (target < f.entry || target >= f.end) {
                        scan.linkReturns.add(a + 8);
                    }
                    else {
                        scan.linkInternal.add(a + 8);
                    }
                }
            }
        }
        return scan;
    }

    // Joins case blocks held as functions to the function whose switch reaches them. A block joins
    // only if every reference to it is that switch's jump, a word of its table, or a non-call flow
    // from the owner or its other case blocks, and if no other entry lies between owner and block
    // (so the block stays inside the owner's [Start, End)). Blocks that own a switch stay.
    private int absorbCaseBlocks(List<Fn> functions, List<Table> tables, long unitStart, long unitEnd) {
        Map<Long, Fn> byEntry = new HashMap<>();
        Map<Fn, Integer> index = new HashMap<>();
        for (int i = 0; i < functions.size(); i++) {
            byEntry.put(functions.get(i).entry, functions.get(i));
            index.put(functions.get(i), i);
        }
        Map<Fn, List<Table>> tablesOf = new LinkedHashMap<>();
        for (Table t : tables) {
            Fn owner = byEntry.get(t.function);
            if (owner != null) {
                tablesOf.computeIfAbsent(owner, k -> new ArrayList<>()).add(t);
            }
        }
        Set<Fn> taken = new HashSet<>();
        for (Map.Entry<Fn, List<Table>> e : tablesOf.entrySet()) {
            Fn owner = e.getKey();
            if (taken.contains(owner)) {
                continue;
            }
            Set<Long> sites = new HashSet<>();
            AddressSet tableData = new AddressSet();
            Set<Fn> blocks = new LinkedHashSet<>();
            for (Table t : e.getValue()) {
                sites.add(t.site);
                tableData.add(toAddr(t.address), toAddr(t.address + 4L * t.targets.size() - 1));
                for (long target : t.targets) {
                    Fn x = byEntry.get(target);
                    if (x != null && x.entry > owner.entry && !taken.contains(x) && !tablesOf.containsKey(x)) {
                        blocks.add(x);
                    }
                }
            }
            boolean changed = true;
            while (changed && !blocks.isEmpty()) {
                changed = false;
                // Keep only the run of blocks that directly follows the owner.
                int i = index.get(owner) + 1;
                Set<Fn> run = new LinkedHashSet<>();
                while (i < functions.size() && blocks.contains(functions.get(i))) {
                    run.add(functions.get(i++));
                }
                if (run.size() != blocks.size()) {
                    blocks = run;
                    changed = true;
                }
                AddressSet allowed = new AddressSet(toAddr(owner.entry), toAddr(owner.end - 1));
                for (Fn x : blocks) {
                    allowed.add(toAddr(x.entry), toAddr(x.end - 1));
                }
                for (Iterator<Fn> it = blocks.iterator(); it.hasNext();) {
                    if (!onlyCaseReferences(it.next(), sites, tableData, allowed, unitStart, unitEnd)) {
                        it.remove();
                        changed = true;
                    }
                }
            }
            for (Fn x : blocks) {
                owner.body.add(x.body);
                taken.add(x);
                absorbed.add(String.format("{ function = %s, case_block = %s }", hex(owner.entry), hex(x.entry)));
            }
        }
        functions.removeIf(taken::contains);
        return taken.size();
    }

    private boolean onlyCaseReferences(Fn x, Set<Long> sites, AddressSetView tableData, AddressSetView allowed,
            long unitStart, long unitEnd) {
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(toAddr(x.entry));
        while (it.hasNext()) {
            Reference r = it.next();
            long from = ramOffset(r.getFromAddress());
            RefType type = r.getReferenceType();
            if (from < unitStart || from >= unitEnd || type.isCall()) {
                return false;
            }
            Address at = toAddr(from);
            if (type.isFlow() ? !sites.contains(from) && !allowed.contains(at) : !tableData.contains(at)) {
                return false;
            }
        }
        return true;
    }

    // An address's offset in main RAM: the EE extension maps the uncached (0x20000000) and
    // uncached-accelerated (0x30000000) mirrors as blocks of their own.
    private static long ramOffset(Address a) {
        long off = a.getOffset();
        return off >= 0x2000_0000L && off < 0x4000_0000L ? off & 0x0FFF_FFFFL : off;
    }

    // Extends ranges so that every flow target of exported code inside the unit is covered, where
    // the target is reached by a run of Ghidra instructions from the end of the range before it.
    // Returns the highest End.
    private long coverFlowTargets(List<Fn> functions, long unitStart, long unitEnd) throws Exception {
        long[] starts = new long[0];
        TreeSet<Long> uncovered = new TreeSet<>();
        Map<Long, Long> sourceOf = new HashMap<>();
        boolean changed = true;
        while (changed) {
            changed = false;
            uncovered.clear();
            starts = new long[functions.size()];
            for (int i = 0; i < starts.length; i++) {
                starts[i] = functions.get(i).entry;
            }
            for (Fn f : functions) {
                monitor.checkCancelled();
                for (long a = f.entry; a < f.end; a += 4) {
                    Instruction ins = listing.getInstructionAt(toAddr(a));
                    if (ins == null) {
                        continue;
                    }
                    for (Reference r : ins.getReferencesFrom()) {
                        if (r.getReferenceType().isFlow() && r.isMemoryReference()) {
                            noteTarget(r.getToAddress().getOffset(), a, functions, starts, unitStart, unitEnd,
                                    uncovered, sourceOf);
                        }
                    }
                }
                // Ghidra stops disassembling after calls it treats as not returning; no fall-through there.
                if (fallsThrough(f) && listing.getInstructionAt(toAddr(f.end)) != null) {
                    noteTarget(f.end, f.end - 4, functions, starts, unitStart, unitEnd, uncovered, sourceOf);
                }
            }
            for (long t : uncovered) {
                int i = owner(starts, t);
                Fn p = i < 0 ? null : functions.get(i);
                if (p != null && t < p.end) {
                    continue;
                }
                long floor = p == null ? unitStart : p.end;
                long limit = i + 1 < functions.size() ? functions.get(i + 1).entry : unitEnd;
                long a = floor;
                while (a < limit && listing.getInstructionAt(toAddr(a)) != null) {
                    a += 4;
                }
                if (p != null && a > t) {
                    extended.add(String.format("{ function = %s, old_end = %s, new_end = %s, target = %s }",
                            hex(p.entry), hex(p.end), hex(a), hex(t)));
                    p.end = a;
                    changed = true;
                    break;
                }
                if (listing.getInstructionAt(toAddr(t)) == null) {
                    continue;
                }
                long first = t;
                while (first - 4 >= floor && listing.getInstructionAt(toAddr(first - 4)) != null) {
                    first -= 4;
                }
                long last = t;
                while (last < limit && listing.getInstructionAt(toAddr(last)) != null) {
                    last += 4;
                }
                Fn island = new Fn(String.format("sub_%08x", first), first,
                        new AddressSet(toAddr(first), toAddr(last - 1)));
                island.end = last;
                functions.add(i + 1, island);
                islands.add(String.format("{ start = %s, end = %s, target = %s, from = %s }", hex(first), hex(last),
                        hex(t), hex(sourceOf.get(t))));
                changed = true;
                break;
            }
        }
        for (long t : uncovered) {
            holes.add(String.format("{ target = %s, from = %s, reason = \"%s\" }", hex(t), hex(sourceOf.get(t)),
                    holeReason(functions, starts, t)));
        }
        long maxEnd = 0;
        for (Fn f : functions) {
            maxEnd = Math.max(maxEnd, f.end);
        }
        return maxEnd;
    }

    // Why a flow target stayed uncovered: the first word between the range before it and the target
    // that Ghidra holds as data or left undefined.
    private String holeReason(List<Fn> functions, long[] starts, long t) {
        int i = owner(starts, t);
        long a = i < 0 ? t : functions.get(i).end;
        for (; a <= t; a += 4) {
            if (listing.getInstructionAt(toAddr(a)) == null) {
                String what = listing.getDefinedDataContaining(toAddr(a)) != null ? "data"
                        : listing.getInstructionContaining(toAddr(a)) != null ? "inside-instruction" : "undefined";
                return (a == t ? "target-" : "gap-") + what + "@" + hex(a);
            }
        }
        return "unknown";
    }

    private static void noteTarget(long t, long from, List<Fn> functions, long[] starts, long unitStart,
            long unitEnd, Set<Long> uncovered, Map<Long, Long> sourceOf) {
        if (t < unitStart || t >= unitEnd) {
            return;
        }
        int i = owner(starts, t);
        if (i >= 0 && t < functions.get(i).end) {
            return;
        }
        uncovered.add(t);
        sourceOf.putIfAbsent(t, from);
    }

    // Index of the function with the greatest entry <= a, or -1.
    private static int owner(long[] starts, long a) {
        int i = java.util.Arrays.binarySearch(starts, a);
        return i >= 0 ? i : -i - 2;
    }

    // Whether execution can run off the end of f's range: its last word is not the delay slot of an
    // unconditional jump or return.
    private boolean fallsThrough(Fn f) {
        if (f.end - f.entry < 8) {
            return true;
        }
        Instruction branch = listing.getInstructionAt(toAddr(f.end - 8));
        if (branch == null) {
            return false;
        }
        RefType flow = branch.getFlowType();
        return !(flow.isTerminal() || (flow.isJump() && flow.isUnConditional()));
    }

    // Ghidra functions whose entry is in the unit, 4-aligned, and starts an instruction.
    private List<Fn> collectFunctions(long unitStart, long unitEnd) throws Exception {
        List<Fn> out = new ArrayList<>();
        long misaligned = 0;
        long noInstruction = 0;
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(toAddr(unitStart), true);
        while (it.hasNext()) {
            monitor.checkCancelled();
            Function f = it.next();
            long entry = f.getEntryPoint().getOffset();
            if (entry >= unitEnd) {
                break;
            }
            if (entry < unitStart || f.isExternal()) {
                continue;
            }
            if ((entry & 3) != 0) {
                misaligned++;
                continue;
            }
            if (listing.getInstructionAt(f.getEntryPoint()) == null) {
                noInstruction++;
                continue;
            }
            out.add(new Fn(f.getName(), entry, f.getBody()));
        }
        put("dropped_misaligned", misaligned);
        put("dropped_no_instruction", noInstruction);
        return out;
    }

    // End = min(body max + 1, next entry, unit end); records cuts and multi-range bodies.
    private long assignEnds(List<Fn> functions, long unitEnd) {
        long clampedToUnit = 0;
        long tooSmall = 0;
        long bodyBytesCut = 0;
        long maxEnd = 0;
        for (int i = 0; i < functions.size(); i++) {
            Fn f = functions.get(i);
            long bodyEnd = f.body.getMaxAddress().getOffset() + 1;
            long next = i + 1 < functions.size() ? functions.get(i + 1).entry : unitEnd;
            long end = Math.min(bodyEnd, Math.min(next, unitEnd));
            if (bodyEnd > unitEnd && next >= unitEnd) {
                clampedToUnit++;
            }
            end &= ~3L;
            f.end = end;
            if (f.body.getNumAddressRanges() > 1) {
                multiRange.add(String.format("{ function = %s, ranges = %d, body_min = %s, body_end = %s }",
                        hex(f.entry), f.body.getNumAddressRanges(), hex(f.body.getMinAddress().getOffset()),
                        hex(bodyEnd)));
            }
            if (bodyEnd > next && next < unitEnd) {
                AddressSet kept = new AddressSet(toAddr(f.entry), toAddr(Math.max(f.entry, end - 1)));
                long cut = f.body.getNumAddresses() - f.body.intersect(kept).getNumAddresses();
                bodyBytesCut += cut;
                overlaps.add(String.format("{ function = %s, next_entry = %s, body_end = %s, body_bytes_cut = %d }",
                        hex(f.entry), hex(next), hex(bodyEnd), cut));
            }
        }
        List<Fn> kept = new ArrayList<>();
        for (Fn f : functions) {
            if (f.end - f.entry < 4) {
                tooSmall++;
                continue;
            }
            kept.add(f);
            maxEnd = Math.max(maxEnd, f.end);
        }
        functions.clear();
        functions.addAll(kept);
        put("dropped_too_small", tooSmall);
        put("end_clamped_to_unit", clampedToUnit);
        put("body_bytes_cut_by_next_entry", bodyBytesCut);
        return maxEnd;
    }

    // The table behind the computed jr at words[i], or null (the site then goes to the unresolved list).
    private Table jumpTable(Fn f, int[] words, int i) throws Exception {
        long site = f.entry + 4L * i;
        int jumpReg = rs(words[i]);
        Set<Long> ghidraTargets = new HashSet<>();
        Instruction ins = listing.getInstructionAt(toAddr(site));
        if (ins == null) {
            return unresolvedSite(site, f, "not-disassembled", 0);
        }
        for (Reference r : ins.getReferencesFrom()) {
            if (r.getReferenceType().isFlow() && r.isMemoryReference()) {
                ghidraTargets.add(r.getToAddress().getOffset());
            }
        }
        if (ghidraTargets.isEmpty()) {
            return unresolvedSite(site, f, "no-ghidra-targets", 0);
        }
        int lw = lastWriter(words, i, jumpReg);
        if (lw < 0) {
            return unresolvedSite(site, f, "no-writer", ghidraTargets.size());
        }
        int lwOp = op(words[lw]);
        if (lwOp != OP_LW && lwOp != OP_LWU) {
            return unresolvedSite(site, f, "writer-not-lw", ghidraTargets.size());
        }
        int base = rs(words[lw]);
        long baseConst = constantPart(words, lw, base, 0);
        if (baseConst == NO_VALUE) {
            return unresolvedSite(site, f, "no-table-address", ghidraTargets.size());
        }
        long table = (baseConst + simm(words[lw])) & 0xFFFF_FFFFL;

        // Entries in table order: words from the table address while they are Ghidra targets.
        List<Long> scanned = new ArrayList<>();
        for (int k = 0; k < MAX_TABLE_ENTRIES; k++) {
            long word = readWordOrNull(table + 4L * k);
            if (word == NO_VALUE || !ghidraTargets.contains(word)) {
                break;
            }
            scanned.add(word);
        }
        if (!new HashSet<>(scanned).equals(ghidraTargets)) {
            return unresolvedSite(site, f, "table-mismatch", ghidraTargets.size());
        }
        // Prefer the slti/sltiu bound when it is consistent with the scan.
        String bound = "scan";
        List<Long> entries = scanned;
        long n = sltiBound(words, lw);
        if (n > 0 && n <= scanned.size() && new HashSet<>(scanned.subList(0, (int) n)).equals(ghidraTargets)) {
            entries = new ArrayList<>(scanned.subList(0, (int) n));
            bound = "slti";
        }
        return new Table(site, f.entry, table, base, entries, bound, recompKey(words, i));
    }

    private Table unresolvedSite(long site, Fn f, String reason, int ghidraTargets) {
        unresolved.add(String.format("{ site = %s, function = %s, reason = \"%s\", ghidra_targets = %d }", hex(site),
                hex(f.entry), reason, ghidraTargets));
        return null;
    }

    // Index of the nearest earlier word (within the lookback) that writes register reg, or -1.
    private static int lastWriter(int[] words, int i, int reg) {
        for (int k = i - 1; k >= 0 && k >= i - WRITER_LOOKBACK; k--) {
            if (dest(words[k]) == reg) {
                return k;
            }
        }
        return -1;
    }

    // The constant part of reg's value just before words[i]: lui, addiu/daddiu, ori and one addu
    // whose other operand is the (scaled) index. NO_VALUE when there is no constant part.
    private static long constantPart(int[] words, int i, int reg, int depth) {
        if (reg == 0) {
            return 0;
        }
        if (depth > 6) {
            return NO_VALUE;
        }
        int k = lastWriter(words, i, reg);
        if (k < 0) {
            return NO_VALUE;
        }
        int w = words[k];
        switch (op(w)) {
            case OP_LUI:
                return (long) (imm(w) << 16);
            case OP_ADDIU:
            case OP_DADDIU:
            case OP_ADDI:
            case OP_DADDI: {
                long v = constantPart(words, k, rs(w), depth + 1);
                return v == NO_VALUE ? NO_VALUE : v + simm(w);
            }
            case OP_ORI: {
                long v = constantPart(words, k, rs(w), depth + 1);
                return v == NO_VALUE ? NO_VALUE : v | imm(w);
            }
            case OP_SPECIAL:
                if (fn(w) == FN_ADDU || fn(w) == FN_DADDU) {
                    long a = constantPart(words, k, rs(w), depth + 1);
                    long b = constantPart(words, k, rt(w), depth + 1);
                    if (a != NO_VALUE && b == NO_VALUE) {
                        return a;
                    }
                    if (b != NO_VALUE && a == NO_VALUE) {
                        return b;
                    }
                }
                return NO_VALUE;
            default:
                return NO_VALUE;
        }
    }

    // Case count from an slti/sltiu within the lookback before the lw, or 0.
    private static long sltiBound(int[] words, int lw) {
        for (int k = lw - 1; k >= 0 && k >= lw - BOUND_LOOKBACK; k--) {
            int o = op(words[k]);
            if (o == OP_SLTIU || o == OP_SLTI) {
                long n = simm(words[k]);
                return n > 0 ? n : 0;
            }
        }
        return 0;
    }

    // The key ps2_recomp's own matcher (control_flow_analyzer.cpp at eb8688b) would compute for this
    // jr, or NO_VALUE when its pattern does not match.
    private static long recompKey(int[] words, int jr) {
        int jumpReg = rs(words[jr]);
        int lw = -1;
        for (int k = jr - 1; k >= 0 && k >= jr - 20; k--) {
            int o = op(words[k]);
            if ((o == OP_LW || o == OP_LWU) && rt(words[k]) == jumpReg) {
                lw = k;
                break;
            }
        }
        if (lw < 0) {
            return NO_VALUE;
        }
        int base = rs(words[lw]);
        int addu = -1;
        for (int k = lw - 1; k >= 0 && k >= lw - 10; k--) {
            if (op(words[k]) == OP_SPECIAL && fn(words[k]) == FN_ADDU && rd(words[k]) == base) {
                addu = k;
                break;
            }
        }
        if (addu < 0) {
            return NO_VALUE;
        }
        int tableBase = rs(words[addu]);
        int index = rt(words[addu]);
        for (int k = addu - 1; k >= 0 && k >= addu - 20; k--) {
            int w = words[k];
            if (op(w) == OP_LUI && (rt(w) == tableBase || rt(w) == index)) {
                long high = ((long) imm(w) << 16) & 0xFFFF_FFFFL;
                long low = 0;
                for (int j = k + 1; j < addu; j++) {
                    int lowWord = words[j];
                    if (rs(lowWord) == rt(w) && rt(lowWord) == rt(w)) {
                        if (op(lowWord) == OP_ADDIU) {
                            low = simm(lowWord);
                        }
                        else if (op(lowWord) == OP_ORI) {
                            low = imm(lowWord);
                        }
                    }
                }
                return (high + low + simm(words[lw])) & 0xFFFF_FFFFL;
            }
        }
        return NO_VALUE;
    }

    // The general-purpose register an instruction word writes, or -1 (none, or not a GPR).
    private static int dest(int w) {
        int o = op(w);
        switch (o) {
            case OP_SPECIAL: {
                int f = fn(w);
                // jr, syscall, break, sync, mthi, mtlo, div, divu, mtsa, traps
                if (f == FN_JR || f == 0x0C || f == 0x0D || f == 0x0F || f == 0x11 || f == 0x13 || f == 0x1A ||
                        f == 0x1B || f == 0x29 || (f >= 0x30 && f <= 0x37)) {
                    return -1;
                }
                return rd(w);
            }
            case OP_REGIMM:
                return isLinkBranch(rt(w)) ? REG_RA : -1;
            case OP_JAL:
                return REG_RA;
            case OP_COP0:
                return rs(w) == 0x00 ? rt(w) : -1;
            case OP_COP1:
                return rs(w) == 0x00 || rs(w) == 0x02 ? rt(w) : -1;
            case OP_COP2:
                return rs(w) == 0x01 || rs(w) == 0x02 ? rt(w) : -1;
            case OP_MMI: {
                int f = fn(w);
                // mthi1, mtlo1, div1, divu1
                if (f == 0x11 || f == 0x13 || f == 0x1A || f == 0x1B) {
                    return -1;
                }
                return rd(w);
            }
            default:
                break;
        }
        if ((o >= 0x08 && o <= 0x0F) || o == OP_DADDI || o == OP_DADDIU || o == 0x1A || o == 0x1B || o == 0x1E ||
                (o >= 0x20 && o <= 0x27) || o == 0x37) {
            return rt(w);
        }
        return -1;
    }

    private static boolean isLinkBranch(int rt) {
        return rt == RT_BLTZAL || rt == RT_BGEZAL || rt == RT_BLTZALL || rt == RT_BGEZALL;
    }

    private static int op(int w) {
        return (w >>> 26) & 0x3F;
    }

    private static int rs(int w) {
        return (w >>> 21) & 0x1F;
    }

    private static int rt(int w) {
        return (w >>> 16) & 0x1F;
    }

    private static int rd(int w) {
        return (w >>> 11) & 0x1F;
    }

    private static int fn(int w) {
        return w & 0x3F;
    }

    private static int imm(int w) {
        return w & 0xFFFF;
    }

    private static int simm(int w) {
        return (short) (w & 0xFFFF);
    }

    private int readWord(long a) throws MemoryAccessException {
        return memory.getInt(toAddr(a));
    }

    private long readWordOrNull(long a) {
        Address at = toAddr(a);
        if (!memory.contains(at) || !memory.getBlock(at).isInitialized()) {
            return NO_VALUE;
        }
        try {
            return memory.getInt(at) & 0xFFFF_FFFFL;
        }
        catch (MemoryAccessException e) {
            return NO_VALUE;
        }
    }

    private void put(String key, long value) {
        counts.put(key, value);
    }

    // Makes a function at every pointer-table word that targets the unit and has none yet (see entry_tables).
    private void createTableEntryFunctions(String spec, long unitStart, long unitEnd) throws Exception {
        if (!spec.startsWith("entry_tables=")) {
            throw new IllegalArgumentException("expected entry_tables=START:END[,...], got " + spec);
        }
        long words = 0;
        for (String part : spec.substring("entry_tables=".length()).split(",")) {
            String[] se = part.split(":");
            long start = Long.decode(se[0]);
            long end = Long.decode(se[1]);
            for (long a = start; a + 4 <= end; a += 4) {
                words++;
                long target = readWordOrNull(a);
                if (target == NO_VALUE || target < unitStart || target >= unitEnd || (target & 3) != 0) {
                    continue;
                }
                Address at = toAddr(target);
                if (getFunctionAt(at) != null) {
                    continue;
                }
                if (listing.getInstructionAt(at) == null) {
                    new DisassembleCommand(at, null, true).applyTo(currentProgram, monitor);
                }
                if (listing.getInstructionAt(at) != null && createFunction(at, null) != null) {
                    tableEntryFunctions.add(target);
                }
            }
        }
        put("entry_table_words", words);
        put("entry_table_functions_created", tableEntryFunctions.size());
    }

    private static String hex(long v) {
        return String.format("0x%08X", v & 0xFFFF_FFFFL);
    }

    private static PrintWriter open(File f) throws Exception {
        File parent = f.getAbsoluteFile().getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        return new PrintWriter(f, "UTF-8");
    }

    private void writeCsv(File out, List<Fn> functions) throws Exception {
        try (PrintWriter w = open(out)) {
            w.println("Name,Start,End,Size");
            for (Fn f : functions) {
                w.printf("%s,%s,%s,%d%n", csvName(f), hex(f.entry), hex(f.end), f.end - f.entry);
            }
        }
    }

    // The name written to the CSV: Ghidra's, unless it is a label name (then Ghidra's default
    // function name), with characters the CSV reader cannot take replaced.
    private static String csvName(Fn f) {
        for (String label : new String[] { "LAB_", "caseD_", "switchD_", "switchdataD_" }) {
            if (f.name.startsWith(label)) {
                return String.format("FUN_%08x", f.entry);
            }
        }
        return f.name.replaceAll("[,\\s\"]", "_");
    }

    private void writeTables(File out, Iterable<Table> tables, long unitStart, long unitEnd) throws Exception {
        try (PrintWriter w = open(out)) {
            w.println("# Jump tables of unit " + hex(unitStart) + "-" + hex(unitEnd) + " for ps2_recomp's");
            w.println("# [jump_tables], written by tools/ghidra/ExportFunctionMap.java from " +
                    currentProgram.getName() + ".");
            w.println("# Keyed by the table's data address; base_register is the lw's base register.");
            for (Table t : tables) {
                w.println();
                w.println("[[jump_tables.table]]");
                w.println("address = \"" + hex(t.address) + "\"");
                w.println("base_register = " + t.baseRegister);
                w.println("entries = [");
                for (int k = 0; k < t.targets.size(); k++) {
                    w.println("  { index = " + k + ", target = \"" + hex(t.targets.get(k)) + "\" },");
                }
                w.println("]");
            }
        }
    }

    private void writeStats(File out, long unitStart, long unitEnd, TreeSet<Long> linkReturns,
            TreeSet<Long> linkInternal) throws Exception {
        try (PrintWriter w = open(out)) {
            w.println("# Function-map statistics of unit " + hex(unitStart) + "-" + hex(unitEnd) +
                    ", written by tools/ghidra/ExportFunctionMap.java.");
            w.println("unit_start = " + hex(unitStart));
            w.println("unit_end = " + hex(unitEnd));
            w.println("program = \"" + currentProgram.getName() + "\"");
            w.println("# bgezal/bltzal (and likely forms, bal included) whose target is outside the function, plus 8.");
            writeList(w, "link_return_entries", linkReturns);
            w.println("# Functions made at entry_tables words that Ghidra had no function for.");
            writeList(w, "entry_table_functions", tableEntryFunctions);
            w.println("# The same for link branches whose target is inside their own function (not entries).");
            writeList(w, "link_branch_internal", linkInternal);
            w.println("# Computed jr sites with an emitted table (bound: slti/sltiu case count, or scan).");
            writeArray(w, "table_sites", tableSites);
            w.println("# Computed jr sites with no emitted table, and why.");
            writeArray(w, "unresolved", unresolved);
            w.println("# Functions whose body runs past the next entry; End stops at that entry.");
            writeArray(w, "overlap", overlaps);
            writeArray(w, "multi_range", multiRange);
            w.println("# Case blocks Ghidra held as functions, joined to the function whose switch reaches them.");
            writeArray(w, "absorbed", absorbed);
            w.println("# Ranges extended over instructions that exported code jumps or falls into.");
            writeArray(w, "extended", extended);
            w.println("# Islands of instructions in no function body that exported code jumps into.");
            writeArray(w, "islands", islands);
            w.println("# Flow targets inside the unit that no range covers (from: the first source seen).");
            writeArray(w, "holes", holes);
            w.println("# Words inside [Start, End) that Ghidra did not disassemble (gaps of multi-range bodies).");
            writeArray(w, "non_instruction_words", nonInstructionWords);
            w.println("# Tables whose address ps2_recomp's own lw/addu/lui matcher would not compute.");
            writeArray(w, "recomp_key_mismatch", keyMismatches);
            w.println();
            w.println("[counts]");
            for (Map.Entry<String, Long> e : counts.entrySet()) {
                boolean address = e.getKey().equals("highest_end");
                w.println(e.getKey() + " = " + (address ? hex(e.getValue()) : e.getValue()));
            }
        }
    }

    private static void writeList(PrintWriter w, String key, TreeSet<Long> values) {
        w.print(key + " = [");
        int k = 0;
        for (long v : values) {
            w.print((k++ % 8 == 0 ? "\n  " : " ") + hex(v) + ",");
        }
        w.println(values.isEmpty() ? "]" : "\n]");
    }

    private static void writeArray(PrintWriter w, String key, List<String> rows) {
        w.println(key + " = [");
        for (String row : rows) {
            w.println("  " + row + ",");
        }
        w.println("]");
    }
}
