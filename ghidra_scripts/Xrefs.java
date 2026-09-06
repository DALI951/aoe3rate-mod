// Xrefs.java — dump all references to selected addresses and matching instructions
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.mem.Memory;
import java.io.File;
import java.io.PrintWriter;

public class Xrefs extends GhidraScript {
    @Override public void run() throws Exception {
        long[] addrs = {0xDCB808, 0xDCB360, 0xDCB804, 0xDCB80C, 0xDCB810};
        PrintWriter w = new PrintWriter(new File(
            "C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled/xrefs_dcb808.txt"));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Address target = space.getAddress(0xDCB808);
        w.println("=== References to 0xDCB808 ===");
        for (ghidra.program.model.symbol.Reference ref : currentProgram.getReferenceManager().getReferencesTo(target)) {
            Address refAddr = ref.getFromAddress();
            long from = refAddr.getOffset();
            Function f = currentProgram.getFunctionManager().getFunctionContaining(refAddr);
            String fname = (f == null) ? "?" : f.getName();
            String asm = "";
            Instruction ins = currentProgram.getListing().getInstructionAt(refAddr);
            if (ins != null) asm = ins.toString();
            w.println(String.format("  from 0x%08x in %s: %s", from, fname, asm));
        }
        w.println();
        for (long a : addrs) {
            Address at = space.getAddress(a);
            w.println("=== SEARCH [0x" + Long.toHexString(a) + "] appearing in code ===");
            int count = 0;
            for (ghidra.program.model.symbol.Reference ref : currentProgram.getReferenceManager().getReferencesTo(at)) {
                Address refAddr = ref.getFromAddress();
                Instruction ins = currentProgram.getListing().getInstructionAt(refAddr);
                if (ins == null) continue;
                count++;
                if (count > 40) { w.println("  ... more"); break; }
                w.println("  " + String.format("0x%08x", refAddr.getOffset()) + ": " + ins.toString());
            }
            w.println("  total refs (any kind): " + currentProgram.getReferenceManager().getReferenceCountTo(at));
        }
        w.close();
        println("xrefs written");
    }
}