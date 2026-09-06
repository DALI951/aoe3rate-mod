// DumpTargeted.java — decompile only the Step 2 address functions first
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.File;
import java.io.PrintWriter;

public class DumpTargeted extends GhidraScript {
    @Override public void run() throws Exception {
        long[] addrs = {0x44EFFF, 0x49A859, 0x4470DB, 0x7E8171, 0x86DA39, 0xB856BC};
        File outDir = new File("C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled");
        outDir.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        PrintWriter big = new PrintWriter(
            new File("C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled/targeted_functions.txt"));
        int done = 0;
        for (long a : addrs) {
            Function f = currentProgram.getFunctionManager().getFunctionAt(
                currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a));
            if (f == null) {
                big.println("=================== 0x" + Long.toHexString(a) + " NO-FUNCTION ===================");
                println("no function at 0x" + Long.toHexString(a));
                continue;
            }
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            String c = (r == null || !r.decompileCompleted())
                ? "// DECOMPILE FAILED: " + f.getName()
                : r.getDecompiledFunction().getC();
            String fn = String.format("%08x_%s.c", f.getEntryPoint().getOffset(), f.getName().replaceAll("[^A-Za-z0-9_.]", "_"));
            PrintWriter w = new PrintWriter(new File(outDir, fn));
            w.println("// Function: " + f.getName() + " @ 0x" + f.getEntryPoint().toString());
            w.println(c); w.close();
            big.println("=================== " + fn + " ===================");
            big.println(c);
            done++;
        }
        big.close(); ifc.dispose();
        println("TARGETED DONE " + done + " functions -> " + outDir);
    }
}
