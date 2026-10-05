using System.Buffers.Binary;
using RecompOne.Recompiler.Symbols;

namespace RecompOne.Recompiler.Analysis;

//still not sure if this works for all cases, seens to do, loosely based on n64recomp, but stupider
public static class JumpTableAnalyzer
{
    private struct RegState
    {
        public uint PrevLui;
        public int PrevAddiuLo;
        public bool ValidLui;
        public bool ValidAddiu;
        public bool ValidAddend;
        public bool ValidLoaded;
        public uint TableVram;
        public bool ValidBody;
        public int BodyDir;
        public bool FromLoad;

        public void Invalidate()
        {
            this = default;
        }
    }
    //also naughty dog gool jt is different
    public static List<JumpTable> Analyze(MipsFunction func, FunctionInfo elf, uint end = 0)
    {
        if (end <= func.Start) end = func.End;
        var regs = new RegState[32];
        var result = new List<JumpTable>();

        foreach (var instr in func.Instructions)
        {
            var op = instr.Word >> 26;
            var fn = instr.Word & 0x3F;
            int rs = instr.Rs, rt = instr.Rt, rd = instr.Rd;

            switch (op)
            {
                case 0:
                    switch (fn)
                    {
                        case 32:
                        case 33:
                            if (rd != 0)
                            {
                                Addu(regs, rs, rt, rd);
                                MarkBody(regs, rs, rt, rd, 1);
                            }

                            break;
                        case 34:
                        case 35:
                            if (rd != 0)
                            {
                                if (IsSeat(regs[rs]) && !IsSeat(regs[rt]) && !regs[rt].ValidLui)
                                {
                                    regs[rd] = regs[rs];
                                    regs[rd].ValidLoaded = false;
                                    regs[rd].ValidBody = true;
                                    regs[rd].BodyDir = -1;
                                }
                                else
                                {
                                    regs[rd].Invalidate();
                                }
                            }

                            break;
                        case 37:
                            if (rd != 0)
                            {
                                if (rs == 0) regs[rd] = regs[rt];
                                else if (rt == 0) regs[rd] = regs[rs];
                                else regs[rd].Invalidate();
                            }

                            break;
                        case 8:
                            if (rs != 31)
                            {
                                var entries = regs[rs].ValidLoaded
                                    ? ReadEntries(elf, regs[rs].TableVram, func, end)
                                    : regs[rs].ValidBody
                                        ? BodyEntries(regs[rs], func, instr.Vram)
                                        : regs[rs].FromLoad
                                            ? BlindEntries(func, instr.Vram)
                                            : [];
                                if (entries.Length > 0)
                                    result.Add(new JumpTable { JrVram = instr.Vram, Entries = entries });
                            }

                            break;
                        default:
                            if (rd != 0) regs[rd].Invalidate();
                            break;
                    }

                    break;

                case 8:
                case 9:
                {
                    var temp = regs[rs];
                    if (!temp.ValidAddiu)
                    {
                        temp.PrevAddiuLo = (int)instr.ImmS;
                        temp.ValidAddiu = true;
                    }
                    else
                    {
                        temp.Invalidate();
                    }

                    if (rt != 0) regs[rt] = temp;
                    break;
                }

                case 15:
                    if (rt != 0)
                    {
                        regs[rt].Invalidate();
                        regs[rt].PrevLui = (uint)instr.ImmU << 16;
                        regs[rt].ValidLui = true;
                    }

                    break;

                case 35:
                    if (rt != 0)
                    {
                        var baseReg = regs[rs];
                        regs[rt].Invalidate();
                        regs[rt].FromLoad = true;
                        if (rs != 29 && baseReg.ValidLui && (baseReg.ValidAddend || baseReg.ValidAddiu))
                        {
                            var imm = instr.ImmS;
                            var nonzero = imm != 0;
                            if (!(nonzero && baseReg.ValidAddiu))
                            {
                                var lo16 = nonzero ? (uint)(int)imm : (uint)baseReg.PrevAddiuLo;
                                regs[rt].TableVram = baseReg.PrevLui + lo16;
                                regs[rt].ValidLoaded = true;
                            }
                        }
                    }

                    break;

                case 2:
                case 4:
                case 5:
                case 6:
                case 7: break;
                case 3:
                    regs[31].Invalidate();
                    break;
                case 16:
                case 18:
                case 40:
                case 41:
                case 42:
                case 43:
                case 46: break;
                default:
                    if (rt != 0) regs[rt].Invalidate();
                    break;
            }
        }

        return result;
    }

    //the add that joins the lui half with the index
    private static void Addu(RegState[] regs, int rs, int rt, int rd)
    {
        var rsLui = regs[rs].ValidLui;
        var rtLui = regs[rt].ValidLui;
        if (rsLui != rtLui)
        {
            var luiSrc = rsLui ? rs : rt;
            regs[rd] = regs[luiSrc];
            regs[rd].ValidAddend = true;
        }
        else if (rs == 0)
        {
            regs[rd] = regs[rt];
        }
        else if (rt == 0)
        {
            regs[rd] = regs[rs];
        }
        else
        {
            regs[rd].Invalidate();
        }
    }

    private static bool IsSeat(in RegState reg)
    {
        return reg.ValidLui && reg.ValidAddiu;
    }

    private static void MarkBody(RegState[] regs, int rs, int rt, int rd, int dir)
    {
        var rsSeat = IsSeat(regs[rs]);
        var rtSeat = IsSeat(regs[rt]);
        if (rsSeat == rtSeat) return;

        regs[rd] = rsSeat ? regs[rs] : regs[rt];
        regs[rd].ValidLoaded = false;
        regs[rd].ValidBody = true;
        regs[rd].BodyDir = dir;
    }

    private const int MaxBodyEntries = 1024;

    private static uint[] BodyEntries(in RegState reg, MipsFunction func, uint jrVram)
    {
        var seat = reg.PrevLui + (uint)reg.PrevAddiuLo;
        if ((seat & 3) != 0 || seat < func.Start || seat >= func.End) return [];

        uint lo, hi;
        if (reg.BodyDir < 0)
        {
            lo = Math.Max(func.Start, jrVram + 8);
            hi = seat;
        }
        else
        {
            lo = seat;
            hi = func.End - 4;
        }

        if (hi < lo) return [];

        var count = (hi - lo) / 4 + 1;
        if (count > MaxBodyEntries) return [];

        var entries = new uint[count];
        for (var i = 0u; i < count; i++) entries[i] = lo + i * 4;
        return entries;
    }

    private static uint[] BlindEntries(MipsFunction func, uint jrVram)
    {
        var instrs = func.Instructions;
        var leaders = new SortedSet<uint>();

        for (var i = 0; i < instrs.Length; i++)
        {
            if (i == 0 || (i >= 2 && instrs[i - 2].HasDelaySlot)) leaders.Add(instrs[i].Vram);

            var instr = instrs[i];
            if (!instr.HasDelaySlot) continue;

            var op = instr.Word >> 26;
            if (op is not (1 or 2 or 4 or 5 or 6 or 7)) continue;

            var tgt = op == 2 ? instr.JumpTarget : instr.BranchTarget;
            if (tgt >= func.Start && tgt < func.End) leaders.Add(tgt);
        }

        leaders.Remove(jrVram + 4);
        if (leaders.Count > MaxBodyEntries) return [];
        return leaders.ToArray();
    }

    private static uint[] ReadEntries(FunctionInfo elf, uint tableVram, MipsFunction func, uint end)
    {
        var entries = new List<uint>();
        var vram = tableVram;
        while (TryReadWord(elf, vram, out var word))
        {
            if (word < func.Start || word >= end) break;
            entries.Add(word);
            vram += 4;
        }

        return entries.ToArray();
    }

    private static bool TryReadWord(FunctionInfo elf, uint vram, out uint value)
    {
        foreach (var sec in elf.DataSections)
        {
            if (sec.IsZero) continue;
            var secEnd = sec.Va + (uint)sec.Data.Length;
            if (vram >= sec.Va && vram + 4 <= secEnd)
            {
                value = BinaryPrimitives.ReadUInt32LittleEndian(sec.Data.AsSpan((int)(vram - sec.Va)));
                return true;
            }
        }

        //shouldnt this be in rodata?
        if (vram >= elf.TextBase && vram + 4 <= elf.TextBase + (uint)elf.TextData.Length)
        {
            value = BinaryPrimitives.ReadUInt32LittleEndian(elf.TextData.AsSpan((int)(vram - elf.TextBase)));
            return true;
        }

        value = 0;
        return false;
    }
}