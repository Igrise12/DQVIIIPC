// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless postScript: answer cross-reference questions about an analysed program without
// re-analysis. One run answers any number of queries and appends them to one text file.
//
//   -postScript QueryRefs.java <output.txt> [outdir=<dir>] [lookback=<n>] <query>...
//
// Queries (addresses are hex, with or without 0x; <fn> is a hex address inside a function or a
// function name):
//   str=<text>          every ASCII occurrence of <text> in initialized memory, the defined data
//                       unit holding it and every reference to that unit (from address, reference
//                       type, containing function)
//   refs=<hex>          every reference to the address
//   range=<hex>-<hex>   every reference into the address range (inclusive)
//   const=<hex>         every lui/ori, lui/addiu and lui/daddiu pair that materialises the 32-bit
//                       value in a register (within lookback= instructions, default 6) and every
//                       aligned little-endian data word equal to it
//   func=<fn>           summary: entry, size, callers, callees, data references with the string
//                       value of referenced string data
//   callers=<fn>[:n]    caller tree to depth n (default 2)
//   callees=<fn>[:n]    callee tree to depth n (default 2)
//   decomp=<fn>         decompiled C to <outdir>/<entry>.c (needs outdir=)
//   dis=<fn>            disassembly listing with inline references to <outdir>/<entry>.asm
//   disrange=<hex>-<hex> disassembly of every instruction in the range, functions or not, to
//                       <outdir>/<start>-<end>.asm
//
// A <fn> address that no function contains (code Ghidra only labelled, such as a qsort comparator
// passed by pointer) gets a function created at that address for the rest of the run; with
// -readOnly the program is not saved, so the project is unchanged.
//
// The console receives counts only. The output file and outdir hold disassembly, decompiled C and
// string values from the program, so they are game-derived content and must stay under work/
// (git-ignored); never paste them into the repository.
// @category DQ8

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.Symbol;

public class QueryRefs extends GhidraScript {

    private Listing listing;
    private Memory memory;
    private ReferenceManager refs;
    private PrintWriter out;
    private File outDir;
    private int lookback = 6;
    private DecompInterface decompiler;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException(
                    "usage: QueryRefs.java <output.txt> [outdir=<dir>] [lookback=<n>] <query>...");
        }
        listing = currentProgram.getListing();
        memory = currentProgram.getMemory();
        refs = currentProgram.getReferenceManager();
        File outFile = new File(args[0]);
        File parent = outFile.getAbsoluteFile().getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(outFile, StandardCharsets.UTF_8, true))) {
            out = w;
            out.println("### QueryRefs on " + currentProgram.getName() + " (" + currentProgram.getLanguageID() + ")");
            for (int i = 1; i < args.length; i++) {
                monitor.checkCancelled();
                String a = args[i];
                int eq = a.indexOf('=');
                if (eq < 0) {
                    throw new IllegalArgumentException("bad query (expected key=value): " + a);
                }
                String key = a.substring(0, eq);
                String value = a.substring(eq + 1);
                switch (key) {
                    case "outdir":
                        outDir = new File(value);
                        outDir.mkdirs();
                        break;
                    case "lookback":
                        lookback = Integer.parseInt(value);
                        break;
                    case "str":
                        queryString(value);
                        break;
                    case "refs":
                        queryRange(addr(value), addr(value), "refs=" + value);
                        break;
                    case "range": {
                        int dash = value.indexOf('-');
                        queryRange(addr(value.substring(0, dash)), addr(value.substring(dash + 1)),
                                "range=" + value);
                        break;
                    }
                    case "const":
                        queryConstant(value);
                        break;
                    case "func":
                        queryFunction(value);
                        break;
                    case "callers":
                    case "callees":
                        queryTree(key, value);
                        break;
                    case "decomp":
                        queryDecompile(value);
                        break;
                    case "dis":
                        queryDisassembly(value);
                        break;
                    case "disrange": {
                        int dash = value.indexOf('-');
                        queryDisassemblyRange(addr(value.substring(0, dash)), addr(value.substring(dash + 1)));
                        break;
                    }
                    default:
                        throw new IllegalArgumentException("unknown query: " + key);
                }
            }
        }
        finally {
            if (decompiler != null) {
                decompiler.dispose();
            }
        }
        println("QueryRefs: " + (args.length - 1) + " arguments processed; appended to " + outFile.getAbsolutePath());
    }

    // ---- str= ----

    private void queryString(String text) throws Exception {
        byte[] pattern = text.getBytes(StandardCharsets.US_ASCII);
        out.println();
        out.println("## str=" + text);
        int hits = 0;
        int refCount = 0;
        for (MemoryBlock block : memory.getBlocks()) {
            if (!block.isInitialized() || !block.isLoaded()) {
                continue;
            }
            Address a = block.getStart();
            while (a != null && a.compareTo(block.getEnd()) <= 0) {
                monitor.checkCancelled();
                a = memory.findBytes(a, block.getEnd(), pattern, null, true, monitor);
                if (a == null) {
                    break;
                }
                hits++;
                Data data = listing.getDefinedDataContaining(a);
                Address unitStart = data == null ? a : data.getAddress();
                Address unitEnd = data == null ? a.add(pattern.length - 1) : data.getMaxAddress();
                out.println("hit " + a + " in " + block.getName() + " unit " + unitStart + "-" + unitEnd +
                        (data == null ? " (no defined data)" : " " + data.getDataType().getName() +
                                (data.hasStringValue() ? " value=" + quote(String.valueOf(data.getValue())) : "")));
                refCount += listReferencesTo(unitStart, unitEnd, "  ");
                a = a.add(1);
            }
        }
        out.println("total hits " + hits + ", references " + refCount);
        println("str=" + text + ": " + hits + " occurrences, " + refCount + " references");
    }

    // ---- refs= / range= ----

    private void queryRange(Address lo, Address hi, String title) throws Exception {
        out.println();
        out.println("## " + title);
        int n = listReferencesTo(lo, hi, "");
        out.println("total references " + n);
        println(title + ": " + n + " references");
    }

    private int listReferencesTo(Address lo, Address hi, String indent) throws Exception {
        int n = 0;
        AddressSet set = new AddressSet(lo, hi);
        AddressIterator targets = refs.getReferenceDestinationIterator(set, true);
        while (targets.hasNext()) {
            monitor.checkCancelled();
            Address to = targets.next();
            ReferenceIterator it = refs.getReferencesTo(to);
            while (it.hasNext()) {
                Reference r = it.next();
                n++;
                Address from = r.getFromAddress();
                Function f = listing.getFunctionContaining(from);
                Instruction instr = listing.getInstructionAt(from);
                out.println(indent + "ref to " + to + " from " + from + " " + r.getReferenceType() +
                        (f == null ? " (no function)" : " in " + describe(f)) +
                        (instr == null ? "" : "  ; " + instr));
            }
        }
        return n;
    }

    // ---- const= ----

    private void queryConstant(String text) throws Exception {
        long value = Long.parseUnsignedLong(strip0x(text), 16) & 0xFFFFFFFFL;
        out.println();
        out.println(String.format("## const=0x%08X", value));
        int pairs = 0;
        int words = 0;
        InstructionIterator it = listing.getInstructions(true);
        List<Instruction> window = new ArrayList<>();
        while (it.hasNext()) {
            monitor.checkCancelled();
            Instruction instr = it.next();
            String mn = mnemonic(instr);
            if (mn.equals("addiu") || mn.equals("ori") || mn.equals("daddiu")) {
                Register dst = instr.getRegister(0);
                Register src = instr.getRegister(1);
                Scalar imm = instr.getScalar(2);
                if (src != null && imm != null) {
                    long lo = mn.equals("ori") ? imm.getUnsignedValue() & 0xFFFF : imm.getSignedValue();
                    for (int i = window.size() - 1; i >= 0; i--) {
                        Instruction lui = window.get(i);
                        if (!mnemonic(lui).equals("lui")) {
                            if (writes(lui, src)) {
                                break;
                            }
                            continue;
                        }
                        Register luiDst = lui.getRegister(0);
                        Scalar hi = lui.getScalar(1);
                        if (luiDst == null || hi == null || !sameRegister(luiDst, src)) {
                            continue;
                        }
                        long v = (((hi.getUnsignedValue() & 0xFFFF) << 16) + lo) & 0xFFFFFFFFL;
                        if (v == value) {
                            pairs++;
                            Function f = listing.getFunctionContaining(instr.getAddress());
                            out.println("pair " + lui.getAddress() + " + " + instr.getAddress() + " -> " +
                                    (dst == null ? "?" : dst.getName()) +
                                    (f == null ? " (no function)" : " in " + describe(f)) + "  ; " + lui + " ; " + instr);
                        }
                        break;
                    }
                }
            }
            window.add(instr);
            if (window.size() > lookback) {
                window.remove(0);
            }
        }
        byte[] le = { (byte) value, (byte) (value >> 8), (byte) (value >> 16), (byte) (value >> 24) };
        for (MemoryBlock block : memory.getBlocks()) {
            if (!block.isInitialized() || !block.isLoaded()) {
                continue;
            }
            Address a = block.getStart();
            while (a != null && a.compareTo(block.getEnd()) <= 0) {
                monitor.checkCancelled();
                a = memory.findBytes(a, block.getEnd(), le, null, true, monitor);
                if (a == null) {
                    break;
                }
                if (a.getOffset() % 4 == 0) {
                    words++;
                    Function f = listing.getFunctionContaining(a);
                    Data data = listing.getDefinedDataContaining(a);
                    out.println("word " + a + " in " + block.getName() +
                            (f != null ? " inside " + describe(f) : data != null ? " data " + data.getDataType().getName() : ""));
                }
                a = a.add(1);
            }
        }
        out.println("total pairs " + pairs + ", data words " + words);
        println(String.format("const=0x%08X: %d register pairs, %d data words", value, pairs, words));
    }

    // ---- func= ----

    private void queryFunction(String text) throws Exception {
        Function f = resolveFunction(text);
        out.println();
        out.println("## func=" + text + " -> " + describe(f));
        out.println("entry " + f.getEntryPoint() + " body " + f.getBody().getMinAddress() + "-" +
                f.getBody().getMaxAddress() + " size " + f.getBody().getNumAddresses() +
                (f.isThunk() ? " thunk" : "") + " signature " + f.getSignature());
        Set<Function> callers = f.getCallingFunctions(monitor);
        Set<Function> callees = f.getCalledFunctions(monitor);
        out.println("callers " + callers.size() + ":");
        for (Function c : sorted(callers)) {
            out.println("  " + describe(c));
        }
        out.println("callees " + callees.size() + ":");
        for (Function c : sorted(callees)) {
            out.println("  " + describe(c));
        }
        int dataRefs = 0;
        InstructionIterator it = listing.getInstructions(f.getBody(), true);
        Set<Address> seen = new HashSet<>();
        out.println("data references:");
        while (it.hasNext()) {
            monitor.checkCancelled();
            Instruction instr = it.next();
            for (Reference r : instr.getReferencesFrom()) {
                if (r.getReferenceType().isFlow() || !r.isMemoryReference()) {
                    continue;
                }
                dataRefs++;
                Address to = r.getToAddress();
                if (!seen.add(to)) {
                    continue;
                }
                Data data = listing.getDataContaining(to);
                Symbol sym = getSymbolAt(to);
                MemoryBlock block = memory.getBlock(to);
                out.println("  " + instr.getAddress() + " -> " + to + " " + r.getReferenceType() +
                        (block == null ? " (unmapped)" : " in " + block.getName()) +
                        (sym == null ? "" : " " + sym.getName()) +
                        (data != null && data.isDefined() ? " " + data.getDataType().getName() : "") +
                        (data != null && data.hasStringValue() ? " value=" + quote(String.valueOf(data.getValue())) : ""));
            }
        }
        out.println("total data references " + dataRefs + " to " + seen.size() + " distinct addresses");
        println("func=" + text + ": " + describe(f) + ", " + callers.size() + " callers, " + callees.size() +
                " callees, " + seen.size() + " data targets");
    }

    // ---- callers= / callees= ----

    private void queryTree(String kind, String text) throws Exception {
        int depth = 2;
        int colon = text.lastIndexOf(':');
        if (colon > 0) {
            depth = Integer.parseInt(text.substring(colon + 1));
            text = text.substring(0, colon);
        }
        Function f = resolveFunction(text);
        out.println();
        out.println("## " + kind + "=" + text + " depth " + depth + " -> " + describe(f));
        Set<Function> visited = new HashSet<>();
        int n = tree(f, kind.equals("callers"), depth, 0, visited);
        out.println("total distinct functions " + n);
        println(kind + "=" + text + ": " + n + " distinct functions within depth " + depth);
    }

    private int tree(Function f, boolean up, int depth, int level, Set<Function> visited) throws Exception {
        monitor.checkCancelled();
        StringBuilder indent = new StringBuilder();
        for (int i = 0; i < level; i++) {
            indent.append("  ");
        }
        boolean first = visited.add(f);
        out.println(indent + describe(f) + (first ? "" : " (seen)"));
        if (!first || level >= depth) {
            return first ? 1 : 0;
        }
        int n = 1;
        Set<Function> next = up ? f.getCallingFunctions(monitor) : f.getCalledFunctions(monitor);
        for (Function g : sorted(next)) {
            n += tree(g, up, depth, level + 1, visited);
        }
        return n;
    }

    // ---- decomp= ----

    private void queryDecompile(String text) throws Exception {
        Function f = resolveFunction(text);
        requireOutDir();
        if (decompiler == null) {
            decompiler = new DecompInterface();
            decompiler.openProgram(currentProgram);
        }
        DecompileResults res = decompiler.decompileFunction(f, 120, monitor);
        File file = new File(outDir, f.getEntryPoint() + ".c");
        try (PrintWriter w = new PrintWriter(file, StandardCharsets.UTF_8)) {
            if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                w.print(res.getDecompiledFunction().getC());
            }
            else {
                w.println("// decompilation failed: " + (res == null ? "null" : res.getErrorMessage()));
            }
        }
        out.println();
        out.println("## decomp=" + text + " -> " + describe(f) + " written to " + file);
        println("decomp=" + text + ": " + describe(f) + " -> " + file.getName());
    }

    // ---- dis= ----

    private void queryDisassembly(String text) throws Exception {
        Function f = resolveFunction(text);
        requireOutDir();
        File file = new File(outDir, f.getEntryPoint() + ".asm");
        int n = writeListing(file, "; " + describe(f) + " body " + f.getBody().getMinAddress() + "-" +
                f.getBody().getMaxAddress(), listing.getInstructions(f.getBody(), true));
        out.println();
        out.println("## dis=" + text + " -> " + describe(f) + " " + n + " instructions written to " + file);
        println("dis=" + text + ": " + describe(f) + ", " + n + " instructions -> " + file.getName());
    }

    private void queryDisassemblyRange(Address lo, Address hi) throws Exception {
        requireOutDir();
        File file = new File(outDir, lo + "-" + hi + ".asm");
        int n = writeListing(file, "; range " + lo + "-" + hi, listing.getInstructions(new AddressSet(lo, hi), true));
        out.println();
        out.println("## disrange=" + lo + "-" + hi + " " + n + " instructions written to " + file);
        println("disrange=" + lo + "-" + hi + ": " + n + " instructions -> " + file.getName());
    }

    private int writeListing(File file, String title, InstructionIterator it) throws Exception {
        int n = 0;
        try (PrintWriter w = new PrintWriter(file, StandardCharsets.UTF_8)) {
            w.println(title);
            while (it.hasNext()) {
                monitor.checkCancelled();
                Instruction instr = it.next();
                n++;
                Symbol label = getSymbolAt(instr.getAddress());
                if (label != null) {
                    w.println(label.getName() + ":");
                }
                StringBuilder line = new StringBuilder();
                line.append(instr.getAddress()).append("  ").append(instr);
                for (Reference r : instr.getReferencesFrom()) {
                    Address to = r.getToAddress();
                    Symbol sym = getSymbolAt(to);
                    Data data = listing.getDataContaining(to);
                    line.append("  ; ").append(r.getReferenceType().getName()).append(' ').append(to);
                    if (sym != null) {
                        line.append(' ').append(sym.getName());
                    }
                    if (data != null && data.hasStringValue()) {
                        line.append(' ').append(quote(String.valueOf(data.getValue())));
                    }
                }
                w.println(line);
            }
        }
        return n;
    }

    // ---- helpers ----

    private Function resolveFunction(String text) {
        List<Function> byName = getGlobalFunctions(text);
        if (!byName.isEmpty()) {
            return byName.get(0);
        }
        Address a = addr(text);
        Function f = listing.getFunctionContaining(a);
        if (f == null && listing.getInstructionAt(a) != null) {
            f = createFunction(a, null);
            println("created function " + (f == null ? "failed" : describe(f)) + " at " + a + " for this run");
        }
        if (f == null) {
            throw new IllegalArgumentException("no function contains " + a + " and no function is named " + text);
        }
        return f;
    }

    private Address addr(String text) {
        return toAddr(Long.parseUnsignedLong(strip0x(text), 16));
    }

    private static String strip0x(String s) {
        return s.startsWith("0x") || s.startsWith("0X") ? s.substring(2) : s;
    }

    private void requireOutDir() {
        if (outDir == null) {
            throw new IllegalArgumentException("decomp= and dis= need outdir=<dir> before them");
        }
    }

    private static String describe(Function f) {
        return f.getName() + "@" + f.getEntryPoint();
    }

    private static List<Function> sorted(Set<Function> set) {
        Map<Address, Function> m = new HashMap<>();
        TreeSet<Address> keys = new TreeSet<>();
        for (Function f : set) {
            m.put(f.getEntryPoint(), f);
            keys.add(f.getEntryPoint());
        }
        List<Function> list = new ArrayList<>();
        for (Address a : keys) {
            list.add(m.get(a));
        }
        return list;
    }

    // The mnemonic without the sleigh's delay-slot marker.
    private static String mnemonic(Instruction instr) {
        String raw = instr.getMnemonicString();
        return raw.startsWith("_") ? raw.substring(1) : raw;
    }

    // Registers are 128-bit based on the EE sleigh; compare base registers.
    private static boolean sameRegister(Register a, Register b) {
        return a != null && b != null && a.getBaseRegister().equals(b.getBaseRegister());
    }

    private static boolean writes(Instruction instr, Register r) {
        if (instr.getNumOperands() == 0) {
            return false;
        }
        Register dst = instr.getRegister(0);
        return sameRegister(dst, r);
    }

    private static String quote(String s) {
        return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n") + "\"";
    }
}
