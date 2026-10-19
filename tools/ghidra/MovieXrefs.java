// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless postScript: locate the code around the movie player in an analysed program and dump
// it for a human to read, without re-analysis.
//
//   -postScript MovieXrefs.java <output dir> [consts=<file>] [str=<text>]... [addr=<hex>]...
//                                            [range=<hex>-<hex>]... [up=<hex>]... [refs=<hex>]...
//                                            [gpoff=<signed hex>]... [depth=<n>] [max=<n>]
//
// Anchors:
//   consts=<file>  one 32-bit hex constant per line; finds every lui/ori and lui/addiu pair that
//                  materialises it in a register and every raw little-endian data word equal to it
//   str=<text>     every raw ASCII occurrence of <text> in initialized memory (case-sensitive) and
//                  the code that references it (or the defined data unit holding it)
//   range=<a>-<b>  every reference into that address range (I/O registers, DMA channels)
//   addr=<hex>     a function to dump regardless of the anchors
//   up=<hex>       a function to dump together with its direct callers, whatever depth= says
//   refs=<hex>     list every reference to that address (from where, type, containing function)
//   gpoff=<hex>    every load or store through the gp register at that signed 16-bit offset (a global the
//                  decompiler shows as <type>Gp<offset>), listed with its mnemonic and containing function
// Every function containing an anchor hit is dumped, plus its callers up to depth= levels
// (default 1), at most max= functions (default 120): callers, callees, the anchor reasons and the
// decompiled C, one <output dir>/<address>.c per function, plus index.txt. The console gets
// addresses and counts only. The constants file and the output directory hold game-derived
// content and stay git-ignored.
// @category DQ8

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.DataUtilities;
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
import ghidra.program.model.symbol.ReferenceManager;

public class MovieXrefs extends GhidraScript {

    private final Map<Function, Set<String>> reasons = new LinkedHashMap<>();
    private Listing listing;
    private Memory memory;
    private ReferenceManager refs;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: MovieXrefs.java <output dir> [consts=<file>] [str=<text>]... "
                    + "[addr=<hex>]... [range=<hex>-<hex>]... [depth=<n>] [max=<n>]");
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        listing = currentProgram.getListing();
        memory = currentProgram.getMemory();
        refs = currentProgram.getReferenceManager();
        int depth = 1;
        int max = 120;
        List<Function> explicit = new ArrayList<>();

        for (int i = 1; i < args.length; i++) {
            String a = args[i];
            if (a.startsWith("consts=")) {
                scanConstants(new File(a.substring(7)));
            } else if (a.startsWith("str=")) {
                scanString(a.substring(4));
            } else if (a.startsWith("range=")) {
                String[] parts = a.substring(6).split("-");
                scanRange(toAddr(Long.parseLong(strip(parts[0]), 16)), toAddr(Long.parseLong(strip(parts[1]), 16)));
            } else if (a.startsWith("addr=")) {
                Address addr = toAddr(Long.parseLong(strip(a.substring(5)), 16));
                Function f = listing.getFunctionContaining(addr);
                if (f == null) {
                    println("addr=" + addr + ": no function there");
                } else {
                    add(f, "requested addr=" + addr);
                    explicit.add(f);
                }
            } else if (a.startsWith("up=")) {
                Address addr = toAddr(Long.parseLong(strip(a.substring(3)), 16));
                Function f = listing.getFunctionContaining(addr);
                if (f == null) {
                    println("up=" + addr + ": no function there");
                } else {
                    add(f, "requested up=" + addr);
                    for (Function caller : f.getCallingFunctions(monitor)) {
                        add(caller, "calls " + f.getName() + " (requested up=)");
                    }
                }
            } else if (a.startsWith("refs=")) {
                Address addr = toAddr(Long.parseLong(strip(a.substring(5)), 16));
                int n = 0;
                for (Reference r : refs.getReferencesTo(addr)) {
                    Function f = listing.getFunctionContaining(r.getFromAddress());
                    println("refs=" + addr + ": from " + r.getFromAddress() + " type " + r.getReferenceType()
                            + (f == null ? " (data or loose code)" : " in " + f.getName()));
                    n++;
                }
                println("refs=" + addr + ": " + n + " references");
            } else if (a.startsWith("gpoff=")) {
                String v = a.substring(6).trim();
                boolean negative = v.startsWith("-");
                long off = Long.parseLong(strip(negative ? v.substring(1) : v), 16);
                scanGpOffset(negative ? -off : off);
            } else if (a.startsWith("depth=")) {
                depth = Integer.parseInt(a.substring(6));
            } else if (a.startsWith("max=")) {
                max = Integer.parseInt(a.substring(4));
            } else {
                throw new IllegalArgumentException("unknown argument: " + a);
            }
        }

        // callers up to depth
        List<Function> frontier = new ArrayList<>(reasons.keySet());
        for (int level = 1; level <= depth; level++) {
            List<Function> next = new ArrayList<>();
            for (Function f : frontier) {
                for (Function caller : f.getCallingFunctions(monitor)) {
                    if (!reasons.containsKey(caller)) {
                        next.add(caller);
                    }
                    add(caller, "calls " + f.getName() + " (level " + level + ")");
                }
            }
            frontier = next;
        }
        println("MovieXrefs: " + reasons.size() + " functions selected (dumping at most " + max + ")");

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter index = new PrintWriter(new File(outDir, "index.txt"), StandardCharsets.UTF_8)) {
            int n = 0;
            for (Map.Entry<Function, Set<String>> e : reasons.entrySet()) {
                if (n++ >= max) {
                    index.println("# max reached; " + (reasons.size() - max) + " functions not dumped");
                    break;
                }
                Function f = e.getKey();
                String name = f.getName();
                Address entry = f.getEntryPoint();
                long size = f.getBody().getNumAddresses();
                List<String> callers = names(f.getCallingFunctions(monitor));
                List<String> callees = names(f.getCalledFunctions(monitor));
                index.println(entry + " size=" + size + " " + name + " | " + String.join("; ", e.getValue())
                        + " | callers=" + callers + " | callees=" + callees);
                File out = new File(outDir, entry.toString().replace(':', '_') + ".c");
                try (PrintWriter w = new PrintWriter(out, StandardCharsets.UTF_8)) {
                    w.println("// " + name + " at " + entry + " size " + size);
                    for (String r : e.getValue()) {
                        w.println("// reason: " + r);
                    }
                    w.println("// callers: " + callers);
                    w.println("// callees: " + callees);
                    DecompileResults res = ifc.decompileFunction(f, 120, monitor);
                    if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                        w.println(res.getDecompiledFunction().getC());
                    } else {
                        w.println("// decompilation failed: " + (res == null ? "null" : res.getErrorMessage()));
                    }
                }
            }
        } finally {
            ifc.dispose();
        }
        println("MovieXrefs: wrote " + Math.min(max, reasons.size()) + " files to " + outDir);
    }

    private static String strip(String hex) {
        String s = hex.trim();
        return s.startsWith("0x") || s.startsWith("0X") ? s.substring(2) : s;
    }

    private static List<String> names(Set<Function> fs) {
        List<String> out = new ArrayList<>();
        for (Function f : new TreeMap<>(mapByAddress(fs)).values()) {
            out.add(f.getName() + "@" + f.getEntryPoint());
        }
        return out;
    }

    private static Map<Address, Function> mapByAddress(Set<Function> fs) {
        Map<Address, Function> m = new LinkedHashMap<>();
        for (Function f : fs) {
            m.put(f.getEntryPoint(), f);
        }
        return m;
    }

    private void add(Function f, String reason) {
        reasons.computeIfAbsent(f, k -> new LinkedHashSet<>()).add(reason);
    }

    private void addReferrers(Address target, String reason, int limit) {
        int count = 0;
        for (Reference r : refs.getReferencesTo(target)) {
            Function f = listing.getFunctionContaining(r.getFromAddress());
            if (f != null) {
                add(f, reason + " from " + r.getFromAddress());
            }
            if (++count >= limit) {
                break;
            }
        }
    }

    private static String mnemonic(Instruction in) {
        String m = in.getMnemonicString();
        return m.startsWith("_") ? m.substring(1) : m; // delay-slot prefix of the EE sleigh
    }

    private static Register base(Register r) {
        return r == null ? null : r.getBaseRegister();
    }

    /** Every lui/ori and lui/addiu pair that builds one of the constants, and every raw data word. */
    private void scanConstants(File file) throws Exception {
        List<Long> consts = new ArrayList<>();
        for (String line : Files.readAllLines(file.toPath())) {
            String s = line.trim();
            if (!s.isEmpty()) {
                consts.add(Long.parseLong(strip(s), 16) & 0xFFFFFFFFL);
            }
        }
        println("constants: " + consts.size() + " read");
        int[] pairHits = new int[consts.size()];
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction lui = it.next();
            if (!mnemonic(lui).equals("lui")) {
                continue;
            }
            Scalar hi = lui.getScalar(1);
            Register dst = lui.getRegister(0);
            if (hi == null || dst == null) {
                continue;
            }
            long hiVal = hi.getUnsignedValue() & 0xFFFF;
            for (int ci = 0; ci < consts.size(); ci++) {
                long c = consts.get(ci);
                long lo = c & 0xFFFF;
                long hiOri = c >>> 16;
                long hiAdd = lo >= 0x8000 ? (hiOri + 1) & 0xFFFF : hiOri;
                long loAdd = lo >= 0x8000 ? lo - 0x10000 : lo;
                if (hiVal != hiOri && hiVal != hiAdd) {
                    continue;
                }
                Instruction in = lui;
                for (int k = 0; k < 12 && in != null; k++) {
                    in = in.getNext();
                    if (in == null) {
                        break;
                    }
                    String m = mnemonic(in);
                    boolean ori = m.equals("ori") && hiVal == hiOri;
                    boolean addiu = m.equals("addiu") && hiVal == hiAdd;
                    if (!ori && !addiu) {
                        continue;
                    }
                    Register src = in.getRegister(1);
                    Scalar imm = in.getScalar(2);
                    if (src == null || imm == null || base(src) != base(dst)) {
                        continue;
                    }
                    long v = ori ? imm.getUnsignedValue() & 0xFFFF : imm.getSignedValue();
                    if ((ori && v == lo) || (addiu && v == loAdd)) {
                        Function f = listing.getFunctionContaining(in.getAddress());
                        pairHits[ci]++;
                        println("constant #" + ci + " built at " + in.getAddress() + " (" + m + ") in "
                                + (f == null ? "no function" : f.getName()));
                        if (f != null) {
                            add(f, "constant #" + ci + " materialised at " + in.getAddress());
                        }
                        break;
                    }
                }
            }
        }
        for (int ci = 0; ci < consts.size(); ci++) {
            long c = consts.get(ci);
            byte[] le = { (byte) c, (byte) (c >>> 8), (byte) (c >>> 16), (byte) (c >>> 24) };
            int dataHits = 0;
            for (MemoryBlock b : memory.getBlocks()) {
                if (!b.isInitialized()) {
                    continue;
                }
                Address a = b.getStart();
                while (a != null && a.compareTo(b.getEnd()) < 0) {
                    a = memory.findBytes(a, b.getEnd(), le, null, true, monitor);
                    if (a == null) {
                        break;
                    }
                    dataHits++;
                    Function holder = listing.getFunctionContaining(a);
                    println("constant #" + ci + " as data word at " + a
                            + (holder == null ? "" : " (inside function " + holder.getName() + ")"));
                    addReferrers(a, "constant #" + ci + " data word at " + a + " referenced", 20);
                    a = a.add(1);
                }
            }
            println("constant #" + ci + ": " + pairHits[ci] + " lui pairs, " + dataHits + " data words");
        }
    }

    /** Raw ASCII occurrences of text and the code referencing them. */
    private void scanString(String text) throws Exception {
        byte[] pat = text.getBytes(StandardCharsets.US_ASCII);
        int hits = 0;
        int referenced = 0;
        for (MemoryBlock b : memory.getBlocks()) {
            if (!b.isInitialized()) {
                continue;
            }
            Address a = b.getStart();
            while (a != null && a.compareTo(b.getEnd()) < 0 && hits < 200) {
                a = memory.findBytes(a, b.getEnd(), pat, null, true, monitor);
                if (a == null) {
                    break;
                }
                hits++;
                Address target = a;
                Data d = DataUtilities.getDataAtAddress(currentProgram, a);
                if (d == null) {
                    d = listing.getDataContaining(a);
                }
                if (d != null) {
                    target = d.getAddress();
                }
                int before = reasons.size();
                addReferrers(target, "string '" + text + "' at " + a, 20);
                if (!target.equals(a)) {
                    addReferrers(a, "string '" + text + "' at " + a, 20);
                }
                if (reasons.size() > before) {
                    referenced++;
                }
                a = a.add(1);
            }
        }
        println("string '" + text + "': " + hits + " occurrences, " + referenced + " with new referencing functions");
    }

    /** Every gp-relative memory access at the given signed offset. */
    private void scanGpOffset(long offset) {
        Register gp = currentProgram.getRegister("gp");
        int count = 0;
        InstructionIterator it = listing.getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction in = it.next();
            String m = mnemonic(in);
            if (!(m.startsWith("s") || m.startsWith("l")) || in.getNumOperands() < 2) {
                continue;
            }
            Object[] ops = in.getOpObjects(1);
            boolean viaGp = false;
            boolean match = false;
            for (Object o : ops) {
                if (o instanceof Register && base((Register) o) == base(gp)) {
                    viaGp = true;
                }
                if (o instanceof Scalar && ((Scalar) o).getSignedValue() == offset) {
                    match = true;
                }
            }
            if (viaGp && match) {
                count++;
                Function f = listing.getFunctionContaining(in.getAddress());
                println("gpoff " + Long.toHexString(offset) + ": " + m + " at " + in.getAddress() + " in "
                        + (f == null ? "no function" : f.getName()));
                if (f != null && m.startsWith("s")) {
                    add(f, "stores gp" + Long.toHexString(offset) + " at " + in.getAddress());
                }
            }
        }
        println("gpoff " + Long.toHexString(offset) + ": " + count + " accesses");
    }

    /** Every reference into [start, end]. */
    private void scanRange(Address start, Address end) {
        int count = 0;
        Set<Function> fs = new LinkedHashSet<>();
        Address a = start;
        while (a != null && a.compareTo(end) <= 0) {
            for (Reference r : refs.getReferencesTo(a)) {
                count++;
                Function f = listing.getFunctionContaining(r.getFromAddress());
                if (f != null) {
                    fs.add(f);
                    add(f, "references " + a + " from " + r.getFromAddress());
                }
            }
            a = a.add(1);
        }
        println("range " + start + "-" + end + ": " + count + " references from " + fs.size() + " functions");
    }
}
