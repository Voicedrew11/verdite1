using RecompOne.Runtime.Hle;

namespace RecompOne.Runtime.Interp;

internal enum GraphOp : byte
{
    DrawEnv,
    Tri,
    Rect,
    Line,
    Fill,
    CopyVram,
    WriteVram
}

internal struct TriRecord
{
    public HleVertex A;
    public HleVertex B;
    public HleVertex C;
    public PrimFlags Flags;
    public int Transform;
    public float OffsetX;
    public float OffsetY;
}

internal struct TransformRecord
{
    public int Serial;
    public uint Key;
    public uint Pages;
    public float U0, U1, V0, V1;
    public int Ord;
    public short R0, R1, R2, R3, R4, R5, R6, R7, R8;
    public int TX, TY, TZ;
    public int H, OFX, OFY;
    public float Vx, Vy, Vz;
    public float Sx, Sy;
    public float Vsx, Vsy;
    public bool Screened;
    public int Tris;
    public int Match;
    public bool Lerp;
    public bool Warpable;
    public int Held;
}

internal struct RectRecord
{
    public HleRect Rect;
    public PrimFlags Flags;
}

internal struct LineRecord
{
    public HleVertex A;
    public HleVertex B;
    public PrimFlags Flags;
}

internal struct FillRecord
{
    public int X;
    public int Y;
    public int W;
    public int H;
    public ushort Color;
}

internal struct CopyRecord
{
    public int Sx;
    public int Sy;
    public int Dx;
    public int Dy;
    public int W;
    public int H;
}

internal struct WriteRecord
{
    public int X;
    public int Y;
    public int W;
    public int H;
    public int Offset;
    public int Length;
}

internal sealed class FrameGraph
{
    public readonly List<GraphOp> Ops = [];
    public readonly List<int> Slots = [];
    public readonly List<HleDrawEnv> Envs = [];
    public readonly List<TriRecord> Tris = [];
    public readonly List<TransformRecord> Transforms = [];
    public readonly List<RectRecord> Rects = [];
    public readonly List<LineRecord> Lines = [];
    public readonly List<FillRecord> Fills = [];
    public readonly List<CopyRecord> Copies = [];
    public readonly List<WriteRecord> Writes = [];
    public readonly List<ushort> Pixels = [];
    
    public bool Interpolatable = true;
    
    public bool IsEmpty => Ops.Count == 0;
    
    public void Clear()
    {
        Ops.Clear();
        Slots.Clear();
        Envs.Clear();
        Tris.Clear();
        Transforms.Clear();
        Rects.Clear();
        Lines.Clear();
        Fills.Clear();
        Copies.Clear();
        Writes.Clear();
        Pixels.Clear();
        Interpolatable = true;
    }
    
    public void Add(GraphOp op, int slot)
    {
        Ops.Add(op);
        Slots.Add(slot);
    }
    
    public int AddPixels(ReadOnlySpan<ushort> pixels)
    {
        var offset = Pixels.Count;
        foreach (var p in pixels) Pixels.Add(p);
        return offset;
    }
}
