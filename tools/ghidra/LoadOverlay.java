// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless postScript: place one MWo3 overlay file in the main program's overlay window and analyse it
// there, so cross-references resolve in both directions (main -> overlay, overlay -> main).
//
//   -postScript LoadOverlay.java <overlay.bin> [<report.csv>]
//
// Run it on a copy of the analysed main-program project (one copy per overlay), without -readOnly
// and with -noanalysis (the script runs the analysis itself on what it changed):
//
//   tools/ghidra/headless.sh work/ghidra/s3-title DQ8 -process SLUS_212.07 -noanalysis -max-cpu 4 \
//       -postScript LoadOverlay.java baserom/raw/BIN/TITLE.BIN work/ghidra/s3-title/overlay-title.csv
//
// What it does, in order:
//   1. parses the 64-byte MWo3 header (magic, id, load address, text/data/bss sizes, constructor table
//      bounds, name) and checks the file-size identity file == 64 + text + data;
//   2. converts the uninitialized block that holds the load address (overlay_window from
//      MapElfSegmentTails.java) to initialized memory, zero-filled, and writes the whole file at the load
//      address, header included, exactly as the game's loader does (code therefore starts at +0x40);
//   3. gives the window the same gp register assumption as the main program, so gp-relative references
//      from overlay code resolve;
//   4. applies a header structure and a pointer array for the table, then seeds code in three passes with
//      an analysis run after each: (1) the constructor-table entries (static-constructor thunks, which sit
//      in the data range after the text); (2) the main program's call targets into the window and the
//      one-byte placeholder functions the base import holds there, but only where they land on bytes no
//      pass-1 function covers, because those 120 addresses are the union over all six overlays and in any
//      one overlay some of them fall inside other code; (3) aligned data words of the main image that point
//      into the overlay's text and still hold no code (function-pointer tables); (4) aligned words in the
//      overlay's own data range that point into its text and still hold no code (vtables and callback
//      tables), which is where most of the code only reachable through pointers is found;
//   5. writes a CSV of counts, addresses and sizes only: header fields, seed counts, functions and
//      instructions in the window, calls and data references between the window and the main program,
//      how each main-program call target and data pointer into the window lands in this overlay (function
//      entry, inside a function, no code), computed jumps and calls, syscall sites, write references into
//      executable code (self-modifying-code candidates) and references into the part of the window beyond
//      this overlay's memory size.
// Nothing from the overlay (bytes, instructions, strings) is written anywhere but the Ghidra project.
// @category DQ8

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.data.ArrayDataType;
import ghidra.program.model.data.CharDataType;
import ghidra.program.model.data.DataType;
import ghidra.program.model.data.PointerDataType;
import ghidra.program.model.data.StructureDataType;
import ghidra.program.model.data.UnsignedIntegerDataType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.lang.RegisterValue;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

public class LoadOverlay extends GhidraScript {

    private static final int HEADER_SIZE = 0x40;
    private static final byte[] MAGIC = { 'M', 'W', 'o', '3' };

    private Listing listing;
    private Memory memory;
    private ReferenceManager refs;
    private FunctionManager functions;

    // Header fields (addresses and sizes only).
    private long overlayId;
    private long loadAddress;
    private long textSize;
    private long dataSize;
    private long bssSize;
    private long tableStart;
    private long tableEnd;
    private long fileSize;

    private Address base;
    private Address textStart;
    private Address textEnd; // inclusive
    private Address dataStart;
    private Address dataEnd; // inclusive
    private Address bssEnd; // inclusive: last byte this overlay owns
    private AddressSet window;
    private AddressSet overlaySet; // header + text + data + bss
    private AddressSet overlayText;
    private AddressSet overlayFile; // header + text + data: everything the file provides
    private AddressSet mainImage; // initialized, loaded blocks below the window
    private AddressSet mainText; // bytes of the main image that hold instructions

    private final Map<String, Long> counts = new LinkedHashMap<>();
    private final List<String[]> rows = new ArrayList<>();

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1 || args.length > 2) {
            throw new IllegalArgumentException("usage: LoadOverlay.java <overlay.bin> [<report.csv>]");
        }
        listing = currentProgram.getListing();
        memory = currentProgram.getMemory();
        refs = currentProgram.getReferenceManager();
        functions = currentProgram.getFunctionManager();

        byte[] file = Files.readAllBytes(new File(args[0]).toPath());
        parseHeader(file);
        MemoryBlock block = placeBytes(file);
        setGpAssumption();

        TreeSet<Address> seeds = new TreeSet<>();
        TreeSet<Address> tableEntries = readTable(file, seeds);
        TreeSet<Address> existingEntries = collectExistingEntries();
        TreeSet<Address> flowTargets = new TreeSet<>();
        TreeSet<Address> dataPointers = new TreeSet<>();
        collectMainReferences(flowTargets, dataPointers);

        clearWindow();
        applyHeaderTypes(tableEntries.size());

        // Pass 1: the constructor thunks. Analysis follows their callees, so the overlay's own call
        // graph is discovered before any address the main program uses is trusted.
        int created = 0;
        for (Address a : seeds) {
            monitor.checkCancelled();
            if (makeFunction(a)) {
                created++;
            }
        }
        println("LoadOverlay: pass 1: " + seeds.size() + " constructor entries, " + created + " functions created; analysing");
        analyzeChanges(currentProgram);

        // Pass 2: the main program's call targets and the placeholder entries, but only where they land on
        // bytes nothing has disassembled yet. The 120 placeholder entries are the union over all six overlays;
        // in this overlay some of them fall inside functions found in pass 1, which marks them as another
        // overlay's entries rather than seeds.
        TreeSet<Address> mainTargets = new TreeSet<>(existingEntries);
        mainTargets.addAll(flowTargets);
        int tried = 0;
        int insideCode = 0;
        int created2 = 0;
        for (Address a : mainTargets) {
            monitor.checkCancelled();
            if (!overlayFile.contains(a) || a.compareTo(textStart) < 0 || listing.getFunctionAt(a) != null) {
                continue; // beyond this overlay's bytes, in its header, or already a function
            }
            if (listing.getInstructionContaining(a) != null) {
                insideCode++;
                continue;
            }
            tried++;
            if (makeFunction(a)) {
                created2++;
            }
        }
        println("LoadOverlay: pass 2: " + mainTargets.size() + " main-program targets, " + tried + " seeded, " + created2 +
                " functions created, " + insideCode + " inside code found in pass 1; analysing");
        analyzeChanges(currentProgram);

        // Pass 3: data words in the main image that point into this overlay's text and landed on no code.
        int pointerSeeds = 0;
        int pointerCreated = 0;
        for (Address a : dataPointers) {
            monitor.checkCancelled();
            if (!overlayText.contains(a) || listing.getInstructionContaining(a) != null) {
                continue;
            }
            pointerSeeds++;
            if (makeFunction(a)) {
                pointerCreated++;
            }
        }
        if (pointerCreated > 0) {
            println("LoadOverlay: pass 3: " + pointerCreated + " of " + pointerSeeds + " pointer seeds became functions; analysing again");
            analyzeChanges(currentProgram);
        }

        // Pass 4: the overlay's own function-pointer tables (C++ vtables, callback tables): aligned words in the
        // data range that point into the text range at bytes still holding no code.
        long coverageBefore = textCoverage();
        int vtableSeeds = 0;
        int vtableCreated = 0;
        for (Address a = dataStart; a.compareTo(dataEnd) < 0; a = a.add(4)) {
            monitor.checkCancelled();
            long word = memory.getInt(a) & 0xFFFFFFFFL;
            if (word % 4 != 0 || word < textStart.getOffset() || word > textEnd.getOffset()) {
                continue;
            }
            Address target = toAddr(word);
            if (listing.getInstructionContaining(target) != null) {
                continue;
            }
            vtableSeeds++;
            if (makeFunction(target)) {
                vtableCreated++;
            }
        }
        if (vtableCreated > 0) {
            println("LoadOverlay: pass 4: " + vtableCreated + " of " + vtableSeeds + " data-range pointer seeds became functions; analysing again");
            analyzeChanges(currentProgram);
        }
        put("pass4_data_pointer_seeds", vtableSeeds);
        put("pass4_functions_created", vtableCreated);
        put("text_bytes_in_functions_before_pass4", coverageBefore);

        put("header_overlay_id", overlayId);
        put("header_load_address", loadAddress);
        put("header_text_size", textSize);
        put("header_data_size", dataSize);
        put("header_bss_size", bssSize);
        put("header_table_start", tableStart);
        put("header_table_end", tableEnd);
        put("header_table_entries", tableEntries.size());
        put("file_size", fileSize);
        put("memsz", HEADER_SIZE + textSize + dataSize + bssSize);
        put("window_block_size", block.getSize());
        put("pass1_table_seeds", seeds.size());
        put("pass1_functions_created", created);
        put("pass2_main_targets", mainTargets.size());
        put("pass2_seeded", tried);
        put("pass2_functions_created", created2);
        put("pass2_inside_pass1_code", insideCode);
        put("pass3_pointer_seeds", pointerSeeds);
        put("pass3_functions_created", pointerCreated);
        report(tableEntries, flowTargets, dataPointers);
        writeCsv(args.length > 1 ? new File(args[1]) : null);
    }

    // ---- 1. header ----

    private void parseHeader(byte[] file) {
        if (file.length < HEADER_SIZE) {
            throw new IllegalArgumentException("file shorter than the 64-byte MWo3 header");
        }
        for (int i = 0; i < 4; i++) {
            if (file[i] != MAGIC[i]) {
                throw new IllegalArgumentException("not an MWo3 file");
            }
        }
        overlayId = u32(file, 0x04);
        loadAddress = u32(file, 0x08);
        textSize = u32(file, 0x0C);
        dataSize = u32(file, 0x10);
        bssSize = u32(file, 0x14);
        tableStart = u32(file, 0x18);
        tableEnd = u32(file, 0x1C);
        fileSize = file.length;
        if (fileSize != HEADER_SIZE + textSize + dataSize) {
            throw new IllegalArgumentException(String.format("file size %d != 64 + text %d + data %d", fileSize,
                    textSize, dataSize));
        }
        if (tableEnd < tableStart || (tableEnd - tableStart) % 4 != 0) {
            throw new IllegalArgumentException("bad constructor table bounds");
        }
        base = toAddr(loadAddress);
        textStart = base.add(HEADER_SIZE);
        textEnd = textStart.add(textSize - 1);
        dataStart = textEnd.add(1);
        dataEnd = dataStart.add(dataSize - 1);
        bssEnd = dataEnd.add(bssSize);
        overlaySet = new AddressSet(base, bssEnd);
        overlayText = new AddressSet(textStart, textEnd);
        overlayFile = new AddressSet(base, dataEnd);
        if (tableStart < dataStart.getOffset() || tableEnd > dataEnd.getOffset() + 1) {
            println("LoadOverlay: warning: constructor table is outside the overlay's data range");
        }
        println(String.format("LoadOverlay: id %d, load %s, text %#x, data %#x, bss %#x, table %#x-%#x (%d entries), file %d bytes",
                overlayId, base, textSize, dataSize, bssSize, tableStart, tableEnd, (tableEnd - tableStart) / 4,
                fileSize));
    }

    // ---- 2. memory ----

    private MemoryBlock placeBytes(byte[] file) throws Exception {
        MemoryBlock block = memory.getBlock(base);
        if (block == null) {
            throw new IllegalStateException("no memory block at " + base + "; run MapElfSegmentTails.java first");
        }
        if (!block.isInitialized()) {
            block = memory.convertToInitialized(block, (byte) 0);
            println("LoadOverlay: converted " + block.getName() + " to initialized memory");
        }
        if (bssEnd.compareTo(block.getEnd()) > 0) {
            throw new IllegalStateException("overlay memsz does not fit in block " + block.getName());
        }
        window = new AddressSet(block.getStart(), block.getEnd());
        // Zero the whole block (a re-run or an earlier overlay may have left bytes), then write the file.
        byte[] zero = new byte[(int) block.getSize()];
        memory.setBytes(block.getStart(), zero);
        memory.setBytes(base, file);

        mainImage = new AddressSet();
        for (MemoryBlock b : memory.getBlocks()) {
            if (b.isInitialized() && b.isLoaded() && b.getEnd().compareTo(base) < 0 && !b.getName().startsWith("_elf")) {
                mainImage.add(b.getStart(), b.getEnd());
            }
        }
        mainText = new AddressSet();
        InstructionIterator it = listing.getInstructions(mainImage, true);
        while (it.hasNext()) {
            monitor.checkCancelled();
            Instruction instr = it.next();
            mainText.add(instr.getMinAddress(), instr.getMaxAddress());
        }
        return block;
    }

    // ---- 3. gp ----

    private void setGpAssumption() {
        Register gp = currentProgram.getRegister("gp");
        if (gp == null) {
            return;
        }
        ProgramContext ctx = currentProgram.getProgramContext();
        List<Function> entry = getGlobalFunctions("entry");
        Address probe = entry.isEmpty() ? currentProgram.getImageBase() : entry.get(0).getEntryPoint();
        RegisterValue mainValue = ctx.getRegisterValue(gp, probe);
        RegisterValue windowValue = ctx.getRegisterValue(gp, textStart);
        if (mainValue == null || !mainValue.hasValue()) {
            println("LoadOverlay: the main program has no gp assumption at " + probe + "; none set for the window");
            put("gp_set", 0);
            return;
        }
        if (windowValue != null && windowValue.hasValue() && windowValue.getUnsignedValue().equals(mainValue.getUnsignedValue())) {
            put("gp_set", 0);
            return;
        }
        try {
            ctx.setRegisterValue(window.getMinAddress(), window.getMaxAddress(), mainValue);
            put("gp_set", 1);
            println("LoadOverlay: set the window's gp assumption from " + probe);
        }
        catch (Exception e) {
            println("LoadOverlay: could not set gp for the window: " + e.getMessage());
            put("gp_set", -1);
        }
    }

    // ---- 4. seeds, types, functions ----

    private TreeSet<Address> readTable(byte[] file, TreeSet<Address> seeds) {
        TreeSet<Address> entries = new TreeSet<>();
        int n = (int) ((tableEnd - tableStart) / 4);
        for (int i = 0; i < n; i++) {
            long offset = tableStart - loadAddress + 4L * i;
            if (offset < HEADER_SIZE || offset + 4 > file.length) {
                println("LoadOverlay: warning: table entry " + i + " lies outside the file");
                continue;
            }
            Address target = toAddr(u32(file, (int) offset));
            entries.add(target);
            if (overlayFile.contains(target) && target.compareTo(textStart) >= 0) {
                seeds.add(target); // the constructor thunks sit in the data range, after the text
            }
            else {
                println("LoadOverlay: warning: table entry " + i + " at " + target + " is outside the file's range");
            }
        }
        return entries;
    }

    private TreeSet<Address> collectExistingEntries() {
        TreeSet<Address> entries = new TreeSet<>();
        FunctionIterator it = functions.getFunctions(window, true);
        while (it.hasNext()) {
            entries.add(it.next().getEntryPoint());
        }
        return entries;
    }

    private void collectMainReferences(TreeSet<Address> flowTargets, TreeSet<Address> dataPointers) throws Exception {
        for (Address to : refs.getReferenceDestinationIterator(window, true)) {
            monitor.checkCancelled();
            ReferenceIterator rit = refs.getReferencesTo(to);
            while (rit.hasNext()) {
                Reference r = rit.next();
                Address from = r.getFromAddress();
                if (!mainImage.contains(from)) {
                    continue; // mirrors, ELF header blocks, the window itself
                }
                if (r.getReferenceType().isFlow()) {
                    flowTargets.add(to);
                }
                else if (r.getReferenceType().isData() && listing.getInstructionAt(from) == null && to.getOffset() % 4 == 0) {
                    dataPointers.add(to);
                }
            }
        }
    }

    private void clearWindow() throws Exception {
        FunctionIterator it = functions.getFunctions(window, true);
        List<Function> old = new ArrayList<>();
        while (it.hasNext()) {
            old.add(it.next());
        }
        for (Function f : old) {
            functions.removeFunction(f.getEntryPoint());
        }
        listing.clearCodeUnits(window.getMinAddress(), window.getMaxAddress(), false, monitor);
        put("placeholder_functions_removed", old.size());
    }

    private void applyHeaderTypes(int tableEntries) throws Exception {
        StructureDataType header = new StructureDataType("MWo3Header", 0);
        header.add(new ArrayDataType(CharDataType.dataType, 4, 1), "magic", null);
        header.add(UnsignedIntegerDataType.dataType, "overlay_id", null);
        header.add(new PointerDataType(), "load_address", null);
        header.add(UnsignedIntegerDataType.dataType, "text_size", null);
        header.add(UnsignedIntegerDataType.dataType, "data_size", null);
        header.add(UnsignedIntegerDataType.dataType, "bss_size", null);
        header.add(new PointerDataType(), "table_start", null);
        header.add(new PointerDataType(), "table_end", null);
        header.add(new ArrayDataType(CharDataType.dataType, 32, 1), "name", null);
        createData(base, header);
        createLabel(base, "MWo3_header", true);
        if (tableEntries > 0) {
            DataType table = new ArrayDataType(new PointerDataType(), tableEntries, 4);
            createData(toAddr(tableStart), table);
            createLabel(toAddr(tableStart), "MWo3_ctor_table", true);
        }
    }

    private long textCoverage() {
        AddressSet bodies = new AddressSet();
        FunctionIterator it = functions.getFunctions(window, true);
        while (it.hasNext()) {
            bodies.add(it.next().getBody());
        }
        return bodies.intersect(overlayText).getNumAddresses();
    }

    private boolean makeFunction(Address a) {
        if (listing.getFunctionAt(a) != null) {
            return false;
        }
        if (listing.getInstructionAt(a) == null && !disassemble(a)) {
            return false;
        }
        Function f = createFunction(a, null);
        return f != null;
    }

    // ---- 5. report ----

    private void report(TreeSet<Address> tableEntries, TreeSet<Address> flowTargets, TreeSet<Address> dataPointers)
            throws Exception {
        // Functions and instructions inside the window.
        long functionCount = 0;
        long functionsInText = 0;
        AddressSet bodies = new AddressSet();
        FunctionIterator fit = functions.getFunctions(window, true);
        while (fit.hasNext()) {
            Function f = fit.next();
            functionCount++;
            if (overlayText.contains(f.getEntryPoint())) {
                functionsInText++;
            }
            bodies.add(f.getBody());
        }
        long instructions = 0;
        long instructionBytes = 0;
        long syscalls = 0;
        long jalr = 0;
        long jrComputed = 0;
        long jrComputedUnresolved = 0;
        long instructionsOutsideText = 0;
        long instructionsInData = 0;
        AddressSet windowCode = new AddressSet();
        InstructionIterator iit = listing.getInstructions(window, true);
        while (iit.hasNext()) {
            monitor.checkCancelled();
            Instruction instr = iit.next();
            instructions++;
            instructionBytes += instr.getLength();
            windowCode.add(instr.getMinAddress(), instr.getMaxAddress());
            if (!overlayText.contains(instr.getAddress())) {
                instructionsOutsideText++;
                if (overlayFile.contains(instr.getAddress())) {
                    instructionsInData++;
                }
            }
            String mn = instr.getMnemonicString();
            if (mn.startsWith("_")) {
                mn = mn.substring(1);
            }
            if (mn.equals("syscall")) {
                syscalls++;
            }
            else if (mn.equals("jalr")) {
                jalr++;
            }
            else if (mn.equals("jr")) {
                FlowType flow = instr.getFlowType();
                if (!(flow.isTerminal() && !flow.isComputed())) {
                    jrComputed++;
                    boolean resolved = false;
                    for (Reference r : instr.getReferencesFrom()) {
                        if (r.getReferenceType().isFlow()) {
                            resolved = true;
                        }
                    }
                    if (!resolved) {
                        jrComputedUnresolved++;
                    }
                }
            }
        }
        put("window_functions", functionCount);
        put("window_functions_in_text", functionsInText);
        put("window_instructions", instructions);
        put("window_instruction_bytes", instructionBytes);
        put("window_instructions_outside_text", instructionsOutsideText);
        put("window_instructions_in_data_range", instructionsInData);
        put("text_bytes_in_functions", bodies.intersect(overlayText).getNumAddresses());
        put("text_size", textSize);
        put("window_syscall_sites", syscalls);
        put("window_jalr", jalr);
        put("window_jr_computed", jrComputed);
        put("window_jr_computed_unresolved", jrComputedUnresolved);

        // References from window code.
        long callsToMain = 0;
        TreeSet<Address> callTargetsMain = new TreeSet<>();
        long jumpsToMain = 0;
        long callsIntra = 0;
        long dataToMainImage = 0;
        long dataToBss = 0;
        long dataToOther = 0;
        long dataIntra = 0;
        long writesToWindowText = 0;
        long writesToMainText = 0;
        long refsBeyondMemsz = 0;
        TreeSet<Address> beyondTargets = new TreeSet<>();
        iit = listing.getInstructions(window, true);
        while (iit.hasNext()) {
            monitor.checkCancelled();
            Instruction instr = iit.next();
            for (Reference r : instr.getReferencesFrom()) {
                if (!r.isMemoryReference()) {
                    continue;
                }
                Address to = r.getToAddress();
                RefType t = r.getReferenceType();
                if (window.contains(to)) {
                    if (!overlaySet.contains(to)) {
                        refsBeyondMemsz++;
                        beyondTargets.add(to);
                    }
                    if (t.isCall()) {
                        callsIntra++;
                    }
                    else if (t.isData()) {
                        dataIntra++;
                    }
                    if (t.isWrite() && windowCode.contains(to)) {
                        writesToWindowText++;
                        rows.add(new String[] { "smc_candidate", instr.getAddress().toString(), to.toString() });
                    }
                }
                else if (mainImage.contains(to)) {
                    if (t.isCall()) {
                        callsToMain++;
                        callTargetsMain.add(to);
                    }
                    else if (t.isJump()) {
                        jumpsToMain++;
                    }
                    else if (t.isData()) {
                        dataToMainImage++;
                    }
                    if (t.isWrite() && mainText.contains(to)) {
                        writesToMainText++;
                        rows.add(new String[] { "smc_candidate", instr.getAddress().toString(), to.toString() });
                    }
                }
                else {
                    MemoryBlock b = memory.getBlock(to);
                    if (b != null && b.getName().equals("bss")) {
                        dataToBss++;
                    }
                    else {
                        dataToOther++;
                    }
                }
            }
        }
        put("window_calls_to_main", callsToMain);
        put("window_calls_to_main_distinct_targets", callTargetsMain.size());
        put("window_jumps_to_main", jumpsToMain);
        put("window_calls_intra", callsIntra);
        put("window_data_refs_intra", dataIntra);
        put("window_data_refs_to_main_image", dataToMainImage);
        put("window_data_refs_to_bss", dataToBss);
        put("window_refs_to_other_blocks", dataToOther);
        put("window_write_refs_into_window_code", writesToWindowText);
        put("window_write_refs_into_main_code", writesToMainText);
        put("window_refs_beyond_overlay_memsz", refsBeyondMemsz);
        for (Address a : beyondTargets) {
            rows.add(new String[] { "ref_beyond_memsz_target", a.toString(), "" });
        }

        // How the main program's targets land in this overlay.
        classify("main_call_target", flowTargets);
        classify("main_data_pointer", dataPointers);
        classify("ctor_table_entry", tableEntries);

        // Write references from the main program into the window (any kind).
        long mainWritesIntoWindow = 0;
        for (Address to : refs.getReferenceDestinationIterator(window, true)) {
            ReferenceIterator rit = refs.getReferencesTo(to);
            while (rit.hasNext()) {
                Reference r = rit.next();
                if (mainImage.contains(r.getFromAddress()) && r.getReferenceType().isWrite()) {
                    mainWritesIntoWindow++;
                    rows.add(new String[] { "main_write_into_window", r.getFromAddress().toString(), to.toString() });
                }
            }
        }
        put("main_write_refs_into_window", mainWritesIntoWindow);
    }

    private void classify(String kind, TreeSet<Address> targets) {
        long entry = 0;
        long inside = 0;
        long noCode = 0;
        long outsideText = 0;
        for (Address a : targets) {
            String how;
            if (!overlayText.contains(a)) {
                outsideText++;
                if (a.compareTo(bssEnd) > 0) {
                    how = "beyond_memsz";
                }
                else if (a.compareTo(textStart) < 0) {
                    how = "header";
                }
                else if (listing.getFunctionAt(a) != null) {
                    how = "function_entry_in_data_range";
                }
                else if (listing.getInstructionContaining(a) != null) {
                    how = "inside_code_in_data_range";
                }
                else {
                    how = a.compareTo(dataEnd) > 0 ? "bss" : "data";
                }
            }
            else if (listing.getFunctionAt(a) != null) {
                entry++;
                how = "function_entry";
            }
            else if (listing.getInstructionContaining(a) != null) {
                inside++;
                how = "inside_code";
            }
            else {
                noCode++;
                how = "no_code";
            }
            rows.add(new String[] { kind, a.toString(), how });
        }
        put(kind + "_count", targets.size());
        put(kind + "_at_function_entry", entry);
        put(kind + "_inside_code", inside);
        put(kind + "_no_code", noCode);
        put(kind + "_outside_text", outsideText);
    }

    private void writeCsv(File out) throws Exception {
        if (out == null) {
            for (Map.Entry<String, Long> e : counts.entrySet()) {
                println("  " + e.getKey() + " = " + e.getValue());
            }
            return;
        }
        File parent = out.getAbsoluteFile().getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        try (PrintWriter w = new PrintWriter(out, StandardCharsets.UTF_8)) {
            w.println("section,key,value");
            w.println("program,name," + currentProgram.getName());
            for (Map.Entry<String, Long> e : counts.entrySet()) {
                String key = e.getKey();
                long v = e.getValue();
                boolean hex = key.startsWith("header_") && !key.endsWith("_id") && !key.endsWith("_entries");
                w.println("count," + key + "," + (hex ? String.format("0x%X", v) : Long.toString(v)));
            }
            for (String[] row : rows) {
                w.println(row[0] + "," + row[1] + "," + row[2]);
            }
        }
        TreeMap<String, Long> sorted = new TreeMap<>(counts);
        println("LoadOverlay: wrote " + out.getAbsolutePath() + " (" + sorted.size() + " counts, " + rows.size() + " rows)");
    }

    private void put(String key, long value) {
        counts.put(key, value);
    }

    private static long u32(byte[] b, int off) {
        return (b[off] & 0xFFL) | ((b[off + 1] & 0xFFL) << 8) | ((b[off + 2] & 0xFFL) << 16) | ((b[off + 3] & 0xFFL) << 24);
    }
}
