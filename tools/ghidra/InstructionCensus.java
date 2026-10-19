// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless postScript: count instruction classes over the whole program and write them as CSV.
//
//   -postScript InstructionCensus.java <output.csv>
//
// The CSV holds counts only (plus Ghidra block names and block address ranges): mnemonic
// histogram, syscall numbers with the $v1 immediate set before each syscall, VU0 micro-mode calls
// (vcallms/vcallmsr), COP2 macro ops, MMI ops, 128-bit lq/sq, cache, ei/di, mfc0/mtc0, sync,
// function statistics (count, FUN_-named share, executable bytes covered), computed jumps
// (jr through a register, as opposed to the jr-ra return) with and without a resolved jump
// table, and memory references grouped by the block they point into.
// No instruction bytes, operands or data values are written.
//
// The Emotion Engine sleigh prefixes the mnemonic of an instruction in a delay slot with '_';
// the prefix is stripped before counting and the delay-slot instructions are counted separately.
// @category DQ8

import java.io.File;
import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class InstructionCensus extends GhidraScript {

    // MIPS major opcode field (bits 31..26) values that mark R5900-specific instruction classes.
    private static final int OP_COP0 = 0x10;
    private static final int OP_COP2 = 0x12;
    private static final int OP_MMI = 0x1C;
    private static final int OP_LQ = 0x1E;
    private static final int OP_SQ = 0x1F;
    private static final int OP_LQC2 = 0x36;
    private static final int OP_SQC2 = 0x3E;

    // How many instructions before a syscall are searched for the $v1 immediate.
    private static final int SYSCALL_LOOKBACK = 4;

    private final Map<String, Long> mnemonics = new TreeMap<>();
    private final Map<String, Long> cop0Mnemonics = new TreeMap<>();
    private final Map<String, Long> cop2Mnemonics = new TreeMap<>();
    private final Map<String, Long> mmiMnemonics = new TreeMap<>();
    private final Map<String, Long> syscalls = new TreeMap<>();
    private final Map<String, Long> perBlock = new TreeMap<>();
    private final Map<String, Long> functionsPerBlock = new TreeMap<>();
    private final Map<String, Long> refsToBlock = new TreeMap<>();
    private final Map<String, Long> totals = new TreeMap<>();

    private Register v1Base;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            throw new IllegalArgumentException("usage: InstructionCensus.java <output.csv>");
        }
        File out = new File(args[0]);
        File parent = out.getAbsoluteFile().getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }

        Register v1 = currentProgram.getRegister("v1");
        v1Base = v1 == null ? null : v1.getBaseRegister();

        Memory memory = currentProgram.getMemory();
        AddressSet executable = new AddressSet();
        for (MemoryBlock block : memory.getBlocks()) {
            if (block.isExecute() && block.isInitialized() && block.isLoaded()) {
                executable.add(block.getStart(), block.getEnd());
            }
        }

        AddressSet functionBodies = new AddressSet();
        long functionCount = 0;
        long autoNamed = 0;
        long thunks = 0;
        long bodyBytes = 0;
        FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
        while (functions.hasNext()) {
            monitor.checkCancelled();
            Function f = functions.next();
            functionCount++;
            if (f.getName().startsWith("FUN_")) {
                autoNamed++;
            }
            if (f.isThunk()) {
                thunks++;
            }
            AddressSetView body = f.getBody();
            bodyBytes += body.getNumAddresses();
            functionBodies.add(body);
            MemoryBlock entryBlock = memory.getBlock(f.getEntryPoint());
            bump(functionsPerBlock, entryBlock == null ? "(no block)" : entryBlock.getName());
        }

        long instructions = 0;
        long instructionBytes = 0;
        long delaySlot = 0;
        long instructionsOutsideFunctions = 0;
        long instructionBytesOutsideFunctions = 0;
        long nops = 0;
        long syscallCount = 0;
        long vcallms = 0;
        long vcallmsr = 0;
        long cop2 = 0;
        long cop2Macro = 0;
        long mmi = 0;
        long mmiParallel = 0;
        long lq = 0;
        long sq = 0;
        long lqc2 = 0;
        long sqc2 = 0;
        long cop0 = 0;
        long cache = 0;
        long ei = 0;
        long di = 0;
        long mfc0 = 0;
        long mtc0 = 0;
        long sync = 0;
        long jrReturn = 0;
        long jrComputed = 0;
        long jrComputedUnresolved = 0;
        long jalr = 0;
        long jalrResolved = 0;
        long undecodableLength = 0;

        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            monitor.checkCancelled();
            Instruction instr = it.next();
            instructions++;
            int length = instr.getLength();
            instructionBytes += length;
            Address at = instr.getAddress();
            if (!functionBodies.contains(at)) {
                instructionsOutsideFunctions++;
                instructionBytesOutsideFunctions += length;
            }
            MemoryBlock block = memory.getBlock(at);
            bump(perBlock, block == null ? "(no block)" : block.getName());

            String mn = mnemonic(instr);
            if (instr.getMnemonicString().startsWith("_")) {
                delaySlot++;
            }
            bump(mnemonics, mn);

            int opcode = majorOpcode(instr);
            if (opcode < 0) {
                undecodableLength++;
            }
            switch (opcode) {
                case OP_COP0:
                    cop0++;
                    bump(cop0Mnemonics, mn);
                    break;
                case OP_COP2:
                    cop2++;
                    bump(cop2Mnemonics, mn);
                    if (mn.equals("vcallms")) {
                        vcallms++;
                    }
                    else if (mn.equals("vcallmsr")) {
                        vcallmsr++;
                    }
                    else if (mn.startsWith("v")) {
                        cop2Macro++;
                    }
                    break;
                case OP_MMI:
                    mmi++;
                    bump(mmiMnemonics, mn);
                    if (mn.startsWith("p")) {
                        mmiParallel++;
                    }
                    break;
                case OP_LQ:
                    lq++;
                    break;
                case OP_SQ:
                    sq++;
                    break;
                case OP_LQC2:
                    lqc2++;
                    break;
                case OP_SQC2:
                    sqc2++;
                    break;
                default:
                    break;
            }

            if (mn.equals("nop")) {
                nops++;
            }
            else if (mn.equals("syscall")) {
                syscallCount++;
                bump(syscalls, syscallNumber(instr));
            }
            else if (mn.equals("cache")) {
                cache++;
            }
            else if (mn.equals("ei")) {
                ei++;
            }
            else if (mn.equals("di")) {
                di++;
            }
            else if (mn.equals("mfc0")) {
                mfc0++;
            }
            else if (mn.equals("mtc0")) {
                mtc0++;
            }
            else if (mn.startsWith("sync")) {
                sync++;
            }
            else if (mn.equals("jr")) {
                FlowType flow = instr.getFlowType();
                if (flow.isTerminal() && !flow.isComputed()) {
                    jrReturn++;
                }
                else {
                    jrComputed++;
                    if (!hasFlowReference(instr)) {
                        jrComputedUnresolved++;
                    }
                }
            }
            else if (mn.equals("jalr")) {
                jalr++;
                if (hasFlowReference(instr)) {
                    jalrResolved++;
                }
            }
        }

        long references = 0;
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferenceIterator(currentProgram.getMinAddress());
        while (refs.hasNext()) {
            monitor.checkCancelled();
            Reference ref = refs.next();
            if (!ref.isMemoryReference()) {
                continue;
            }
            references++;
            MemoryBlock target = memory.getBlock(ref.getToAddress());
            bump(refsToBlock, target == null ? "(unmapped)" : target.getName());
        }

        long executableBytes = executable.getNumAddresses();
        long coveredExecutableBytes = functionBodies.intersect(executable).getNumAddresses();

        put("instructions", instructions);
        put("instruction_bytes", instructionBytes);
        put("instructions_in_delay_slots", delaySlot);
        put("instructions_outside_functions", instructionsOutsideFunctions);
        put("instruction_bytes_outside_functions", instructionBytesOutsideFunctions);
        put("instructions_not_4_bytes", undecodableLength);
        put("nop", nops);
        put("functions", functionCount);
        put("functions_fun_named", autoNamed);
        put("functions_thunk", thunks);
        put("function_body_bytes", bodyBytes);
        put("executable_bytes", executableBytes);
        put("executable_bytes_in_functions", coveredExecutableBytes);
        put("memory_references", references);
        put("syscall", syscallCount);
        put("vcallms", vcallms);
        put("vcallmsr", vcallmsr);
        put("cop2_total", cop2);
        put("cop2_macro_ops", cop2Macro);
        put("lqc2", lqc2);
        put("sqc2", sqc2);
        put("mmi_total", mmi);
        put("mmi_parallel_ops", mmiParallel);
        put("lq", lq);
        put("sq", sq);
        put("cop0_total", cop0);
        put("cache", cache);
        put("ei", ei);
        put("di", di);
        put("mfc0", mfc0);
        put("mtc0", mtc0);
        put("sync", sync);
        put("jr_return", jrReturn);
        put("jr_computed", jrComputed);
        put("jr_computed_unresolved", jrComputedUnresolved);
        put("jalr", jalr);
        put("jalr_resolved", jalrResolved);

        try (PrintWriter w = new PrintWriter(out)) {
            w.println("section,key,value");
            w.println("program,name," + currentProgram.getName());
            w.println("program,language," + currentProgram.getLanguageID());
            w.println("program,compiler," + currentProgram.getCompilerSpec().getCompilerSpecID());
            for (MemoryBlock block : memory.getBlocks()) {
                w.println("block," + csv(block.getName()) + "," + block.getStart() + "-" + block.getEnd() + "/" +
                        block.getSize() + "/" + (block.isInitialized() ? "init" : "uninit") +
                        (block.isExecute() ? "+x" : "") + (block.isOverlay() ? "+overlay" : ""));
            }
            write(w, "total", totals);
            write(w, "block_instructions", perBlock);
            write(w, "block_function_entries", functionsPerBlock);
            write(w, "block_references_to", refsToBlock);
            write(w, "syscall_v1", syscalls);
            write(w, "cop0", cop0Mnemonics);
            write(w, "cop2", cop2Mnemonics);
            write(w, "mmi", mmiMnemonics);
            write(w, "mnemonic", mnemonics);
        }

        println(String.format("InstructionCensus: %d instructions, %d functions (%d FUN_-named), " +
                "%d/%d executable bytes in functions, %d syscall sites, %d vcallms, %d vcallmsr, " +
                "%d COP2, %d MMI, %d computed jr (%d unresolved), %d memory references; wrote %s",
                instructions, functionCount, autoNamed, coveredExecutableBytes, executableBytes,
                syscallCount, vcallms, vcallmsr, cop2, mmi, jrComputed, jrComputedUnresolved, references,
                out.getAbsolutePath()));
    }

    // The mnemonic without the sleigh's delay-slot marker.
    private static String mnemonic(Instruction instr) {
        String raw = instr.getMnemonicString();
        return raw.startsWith("_") ? raw.substring(1) : raw;
    }

    // The major opcode of a 4-byte little-endian MIPS instruction, or -1 when the length is not 4.
    private static int majorOpcode(Instruction instr) {
        try {
            byte[] b = instr.getBytes();
            if (b.length != 4) {
                return -1;
            }
            return (b[3] >> 2) & 0x3F;
        }
        catch (Exception e) {
            return -1;
        }
    }

    // The signed immediate the preceding li/addiu/ori/daddiu wrote to $v1, or a reason it was not found.
    private String syscallNumber(Instruction syscall) {
        Instruction p = syscall.getPrevious();
        for (int i = 0; i < SYSCALL_LOOKBACK && p != null; i++, p = p.getPrevious()) {
            if (writesV1(p)) {
                String mn = mnemonic(p);
                if (mn.equals("li") || mn.equals("addiu") || mn.equals("ori") || mn.equals("daddiu")) {
                    Scalar s = p.getScalar(p.getNumOperands() - 1);
                    if (s != null) {
                        long v = s.getSignedValue();
                        return v < 0 ? String.format("-0x%X", -v) : String.format("0x%X", v);
                    }
                }
                return "set-by-" + mn;
            }
        }
        return "not-found";
    }

    private boolean writesV1(Instruction instr) {
        if (v1Base == null || instr.getNumOperands() == 0) {
            return false;
        }
        Register r = instr.getRegister(0);
        return r != null && v1Base.equals(r.getBaseRegister());
    }

    private static boolean hasFlowReference(Instruction instr) {
        for (Reference ref : instr.getReferencesFrom()) {
            if (ref.getReferenceType().isFlow()) {
                return true;
            }
        }
        return false;
    }

    private void put(String key, long value) {
        totals.put(key, value);
    }

    private static void bump(Map<String, Long> map, String key) {
        map.merge(key, 1L, Long::sum);
    }

    private static void write(PrintWriter w, String section, Map<String, Long> map) {
        for (Map.Entry<String, Long> e : map.entrySet()) {
            w.println(section + "," + csv(e.getKey()) + "," + e.getValue());
        }
    }

    private static String csv(String s) {
        return s.contains(",") || s.contains("\"") ? "\"" + s.replace("\"", "\"\"") + "\"" : s;
    }
}
