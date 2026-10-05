namespace RecompOne.Runtime.Hle;

public interface IGlVram
{
    /// <summary>The scaled framebuffer the display targets write back into, and
    /// the present's VRAM fallback. Not what the prim shader samples.</summary>
    uint Texture { get; }
    uint Fbo { get; }

    /// <summary>0054. 1x VRAM the prim shader samples. Never a draw
    /// attachment: <see cref="WriteRect"/> is <c>TexSubImage2D</c> into this, and
    /// the next <c>Flush</c> samples it. Leaving it on <see cref="SampleFbo"/>
    /// was the remaining 0.6 ms stall — the driver treated the upload as a
    /// render-target write. A read attachment is only bound for a blit or
    /// <see cref="ReadRect"/> and detached before return.</summary>
    uint SampleTexture { get; }

    /// <summary>1x FBO for GPU writes (fill, copy, writeback, a draw with no
    /// display target). A different texture from <see cref="SampleTexture"/>, so
    /// sampling pages is not a framebuffer-as-texture hazard.</summary>
    uint SampleFbo { get; }

    void Init();
    void BindDraw();

    /// <summary>Bind <see cref="SampleFbo"/> at 1x. Copies sample VRAM into it
    /// first so a GPU write is not drawing on a stale 1x atlas.</summary>
    void BindSampleDraw();

    /// <summary>The texture to bind as <c>uVram</c> while <see cref="SampleFbo"/>
    /// is the draw target: <see cref="SampleTexture"/> itself.</summary>
    uint BeginSampleRead();

    /// <summary>Copy a GPU write from <see cref="SampleFbo"/> back into
    /// <see cref="SampleTexture"/> so the next batch samples it.</summary>
    void CommitDraw(int x, int y, int w, int h);

    uint BeginDestRead(uint targetTex, int targetW, int targetH, int x, int y, int w, int h);

    void SampleBarrier();

    void Fill(int x, int y, int w, int h, ushort color15);
    void CopyRect(int sx, int sy, int dx, int dy, int w, int h);
    void WriteRect(int x, int y, int w, int h, ReadOnlySpan<ushort> px);
    void ReadRect(int x, int y, int w, int h, Span<ushort> dst);

    /// <summary>Upscale 1x sample VRAM into the scaled framebuffer. A draw with
    /// no display target lands in 1x and then promotes, so the present's VRAM
    /// fallback and 0039's snapshots still see it. Uploads do not, which is the
    /// stall <c>0054</c> removed.</summary>
    void Promote(int x, int y, int w, int h);

    /// <summary>Downsample the scaled framebuffer into 1x sample VRAM. Writeback
    /// of a display target and a scaled snapshot restore use this. A draw with no
    /// display target must not: the scaled atlas no longer holds the uploads, so
    /// publishing a draw AABB would erase the texture pages the shader samples.</summary>
    void Publish(int x, int y, int w, int h);

    /// <summary>Blit 1x sample VRAM into <paramref name="dstFbo"/>, upscaling if
    /// the destination is larger. Used to push a CPU upload into a display
    /// target.</summary>
    void BlitSample(int sx, int sy, int sw, int sh, uint dstFbo, int dstW, int dstH, int dx, int dy, int dw, int dh);

    void Dispose();
}