// MemDump.java — dump bytes at key addresses to verify constants
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.mem.Memory;
import java.io.File;
import java.io.PrintWriter;

public class MemDump extends GhidraScript {
    @Override public void run() throws Exception {
        long[][] ranges = {
            {0xC6DF10, 0x30},
            {0xC6DF38, 0x0C},
            {0xB856BC, 0x10},
            {0xB84BE4, 0x10},
            {0xDCB360, 0x20},
            {0xDCB808, 0x10},
        };
        PrintWriter w = new PrintWriter(new File(
            "C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled/memdump_constants.txt"));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Memory mem = currentProgram.getMemory();
        for (long[] r : ranges) {
            long baseA = r[0], len = r[1];
            w.println(String.format("=== 0x%x size 0x%x ===", baseA, len));
            for (int off = 0; off < len; off += 16) {
                StringBuilder sb = new StringBuilder(String.format("  +0x%03x: ", off));
                for (int k = 0; k < 16 && (off + k) < len; k++) {
                    Address a = space.getAddress(baseA + off + k);
                    try {
                        byte b = mem.getByte(a);
                        sb.append(String.format("%02X ", b & 0xff));
                    } catch (Exception e) {
                        sb.append("?? ");
                    }
                }
                w.println(sb);
            }
        }
        w.close();
        println("memdump written");
    }
}