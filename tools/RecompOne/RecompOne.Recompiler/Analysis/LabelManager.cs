using RecompOne.Recompiler.Disasm;

namespace RecompOne.Recompiler.Analysis;

public static class LabelManager
{
    public static HashSet<uint> Collect(MipsFunction func)
    {
        var labels = new HashSet<uint>();

        foreach (var instr in func.Instructions)
        {
            if (!instr.HasDelaySlot) continue;

            var op = instr.Word >> 26;

            if (op is 1 or 4 or 5 or 6 or 7) //these: REGIMM, BEQ, BNE, BLEZ, BGTZ
            {
                var t = instr.BranchTarget;
                if (t >= func.Start && t < func.End) labels.Add(t);
            }
            else if (op == 2) //internal function goto
            {
                var t = instr.JumpTarget;
                if (t >= func.Start && t < func.End) labels.Add(t);
            }
        }

        foreach (var jtbl in func.JumpTables)
        foreach (var entry in jtbl.Entries)
            if (entry >= func.Start && entry < func.End)
                labels.Add(entry);

        foreach (var entry in CollectLinkReturns(func)) labels.Add(entry);

        return labels;
    }

    public static HashSet<uint> CollectLinkReturns(MipsFunction func)
    {
        var returns = new HashSet<uint>();
        var branched = new HashSet<uint>();

        foreach (var instr in func.Instructions)
        {
            if (!instr.HasDelaySlot) continue;

            var op = instr.Word >> 26;
            if (op == 2) branched.Add(instr.JumpTarget);
            else if (op is 4 or 5 or 6 or 7) branched.Add(instr.BranchTarget);
            else if (op == 1 && (uint)instr.Rt is not (0x10 or 0x11)) branched.Add(instr.BranchTarget);
        }

        foreach (var instr in func.Instructions)
        {
            if (!instr.HasDelaySlot) continue;

            var op = instr.Word >> 26;
            uint target;

            if (op == 3) target = instr.JumpTarget;
            else if (op == 1 && (uint)instr.Rt is 0x10 or 0x11) target = instr.BranchTarget;
            else continue;

            if (target < func.Start || target >= func.End || !branched.Contains(target)) continue;

            var back = instr.Vram + 8;
            if (back >= func.Start && back < func.End) returns.Add(back);
        }

        return returns;
    }

    //back edge is branch that goes backward, usually for loop, this will be used to dispatch some of the irqs that cant be properly hled
    public static HashSet<uint> CollectBackEdges(MipsFunction func)
    {
        var heads = new HashSet<uint>();

        foreach (var instr in func.Instructions)
        {
            if (!instr.HasDelaySlot) continue;

            var op = instr.Word >> 26;
            uint t;

            if (op is 1 or 4 or 5 or 6 or 7) t = instr.BranchTarget;
            else if (op == 2) t = instr.JumpTarget;
            else continue;

            if (t >= func.Start && t < func.End && t <= instr.Vram) heads.Add(t);
        }

        return heads;
    }
}