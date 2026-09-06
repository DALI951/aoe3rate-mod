// DumpAllFunctions.java — no package; run with analyzeHeadless -postScript
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.File;
import java.io.PrintWriter;

public class DumpAllFunctions extends GhidraScript {
    @Override public void run() throws Exception {
        File outDir = new File("C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled");
        outDir.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        PrintWriter big = new PrintWriter(
            new File("C:/Users/Dali/Documents/gaames/ghidra_proj/aoe3y_decompiled/dump_all_functions.txt"));
        int i = 0;
        for (FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
                it.hasNext(); ) {
            Function f = it.next();
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            String c = (r == null || !r.decompileCompleted())
                ? "// DECOMPILE FAILED: " + f.getName()
                : r.getDecompiledFunction().getC();
            String fn = String.format("%08x_%s.c", f.getEntryPoint().getOffset(), sanitize(f.getName()));
            PrintWriter w = new PrintWriter(new File(outDir, fn));
            w.println("// Function: " + f.getName() + " @ 0x" + f.getEntryPoint().toString());
            w.println(c); w.close();
            big.println("=================== " + fn + " ===================");
            big.println(c);
            if (monitor.isCancelled()) break;
            if ((++i % 100) == 0) println("decompiled " + i + " functions");
        }
        big.close(); ifc.dispose();
        println("DONE " + i + " functions -> " + outDir);
    }
    static String sanitize(String n) { return n.replaceAll("[^A-Za-z0-9_.]", "_"); }
}
