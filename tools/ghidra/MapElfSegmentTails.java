// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
//
// Headless preScript: map the parts of ELF PT_LOAD segments that have memory size but no file
// bytes (bss tails and zero-file-size segments) as uninitialized memory blocks.
//
//   -preScript MapElfSegmentTails.java [<hex start address>=<block name>]...
//
// Ghidra's ELF loader creates these regions only when the file has no section headers; a PS2
// boot ELF with an empty section table but present section headers gets neither its bss nor the
// windows that zero-file-size segments reserve, so references into them stay unresolved. This
// script reads the program-header table Ghidra keeps in its "_elfProgramHeaders" block, merges
// overlapping tails into one range, skips anything already mapped and creates one uninitialized
// read/write block per remaining range (executable when a contributing segment is). Blocks are
// named "tail_<start>" unless an argument gives a name for that start address. Only header
// fields (addresses and sizes) are read; nothing from the segments themselves.
// @category DQ8

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class MapElfSegmentTails extends GhidraScript {

    private static final String HEADERS_BLOCK = "_elfProgramHeaders";
    private static final int PHDR_SIZE = 32; // Elf32_Phdr
    private static final int PT_LOAD = 1;
    private static final int PF_X = 1;
    private static final int PF_W = 2;
    private static final int PF_R = 4;

    @Override
    public void run() throws Exception {
        Map<Long, String> names = new HashMap<>();
        for (String arg : getScriptArgs()) {
            int eq = arg.indexOf('=');
            if (eq <= 0 || eq == arg.length() - 1) {
                throw new IllegalArgumentException("expected <hex address>=<name>, got: " + arg);
            }
            names.put(Long.decode(arg.substring(0, eq)), arg.substring(eq + 1));
        }

        Memory memory = currentProgram.getMemory();
        MemoryBlock headers = memory.getBlock(HEADERS_BLOCK);
        if (headers == null) {
            println("MapElfSegmentTails: no " + HEADERS_BLOCK + " block; nothing to do");
            return;
        }
        boolean bigEndian = currentProgram.getLanguage().isBigEndian();
        int count = (int) (headers.getSize() / PHDR_SIZE);
        byte[] table = new byte[count * PHDR_SIZE];
        headers.getBytes(headers.getStart(), table);

        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        List<long[]> tails = new ArrayList<>(); // {start, end inclusive, executable}
        AddressSet executableTails = new AddressSet();
        for (int i = 0; i < count; i++) {
            int base = i * PHDR_SIZE;
            long type = u32(table, base, bigEndian);
            long vaddr = u32(table, base + 8, bigEndian);
            long filesz = u32(table, base + 16, bigEndian);
            long memsz = u32(table, base + 20, bigEndian);
            long flags = u32(table, base + 24, bigEndian);
            if (type != PT_LOAD || memsz <= filesz) {
                continue;
            }
            long start = vaddr + filesz;
            long end = vaddr + memsz - 1;
            boolean execute = (flags & PF_X) != 0;
            tails.add(new long[] { start, end, execute ? 1 : 0 });
            if (execute) {
                executableTails.add(space.getAddress(start), space.getAddress(end));
            }
            println(String.format("MapElfSegmentTails: segment %d has %d bytes without file data at %s-%s%s",
                    i, memsz - filesz, space.getAddress(start), space.getAddress(end), execute ? " (x)" : ""));
        }

        // Merge tails that overlap (several segments reserving one window); keep merely adjacent
        // tails apart so a bss and a following window stay separate blocks.
        tails.sort(Comparator.comparingLong(t -> t[0]));
        AddressSet merged = new AddressSet();
        List<long[]> ranges = new ArrayList<>();
        for (long[] t : tails) {
            if (!ranges.isEmpty() && t[0] <= ranges.get(ranges.size() - 1)[1]) {
                long[] last = ranges.get(ranges.size() - 1);
                last[1] = Math.max(last[1], t[1]);
            }
            else {
                ranges.add(new long[] { t[0], t[1] });
            }
        }

        AddressSet mapped = new AddressSet();
        for (MemoryBlock block : memory.getBlocks()) {
            if (block.isLoaded()) {
                mapped.add(block.getStart(), block.getEnd());
            }
        }

        int created = 0;
        for (long[] r : ranges) {
            AddressSet missing = new AddressSet(space.getAddress(r[0]), space.getAddress(r[1])).subtract(mapped);
            for (AddressRange range : missing) {
                Address start = range.getMinAddress();
                String name = names.getOrDefault(start.getOffset(), "tail_" + start);
                boolean execute = executableTails.intersects(start, range.getMaxAddress());
                MemoryBlock block = memory.createUninitializedBlock(name, start, range.getLength(), false);
                block.setRead(true);
                block.setWrite(true);
                block.setExecute(execute);
                block.setSourceName("MapElfSegmentTails");
                block.setComment("ELF segment memory without file data (uninitialized at load)");
                merged.add(range);
                created++;
                println(String.format("MapElfSegmentTails: created %s %s-%s (%d bytes%s)", name, start,
                        range.getMaxAddress(), range.getLength(), execute ? ", executable" : ""));
            }
        }
        if (created == 0) {
            println("MapElfSegmentTails: every segment tail is already mapped");
        }
    }

    private static long u32(byte[] b, int off, boolean bigEndian) {
        long v = 0;
        for (int i = 0; i < 4; i++) {
            int shift = bigEndian ? (3 - i) * 8 : i * 8;
            v |= (long) (b[off + i] & 0xFF) << shift;
        }
        return v;
    }
}
