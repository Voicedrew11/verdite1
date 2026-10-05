using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

public sealed class Gl45Vram : IGlVram
{
    private readonly GL _gl;
    private uint _tex, _fbo;
    private uint _gpuTex, _gpuFbo;
    private uint _sampleTex;
    private uint _readFbo;
    private uint _scratchTex;

    public uint Texture => _tex;
    public uint Fbo => _fbo;
    public uint SampleTexture => _sampleTex;
    public uint SampleFbo => _gpuFbo;

    public Gl45Vram(GL gl)
    {
        _gl = gl;
    }

    public void Init()
    {
        _tex = CreateTex(GlVram.Width, GlVram.Height);
        _fbo = CreateFbo(_tex);
        _gpuTex = CreateTex(VramShadow.Width, VramShadow.Height);
        _gpuFbo = CreateFbo(_gpuTex);
        _sampleTex = CreateTex(VramShadow.Width, VramShadow.Height);
        _readFbo = _gl.GenFramebuffer();
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    private uint CreateTex(int w, int h)
    {
        var t = _gl.GenTexture();
        _gl.BindTexture(TextureTarget.Texture2D, t);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMinFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureMagFilter, (int)GLEnum.Nearest);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapS, (int)GLEnum.ClampToEdge);
        _gl.TexParameter(TextureTarget.Texture2D, TextureParameterName.TextureWrapT, (int)GLEnum.ClampToEdge);
        _gl.TexImage2D<ushort>(TextureTarget.Texture2D, 0, InternalFormat.Rgb5A1, (uint)w, (uint)h, 0,
            PixelFormat.Rgba, PixelType.UnsignedShort1555Rev, new ushort[w * h].AsSpan());
        return t;
    }

    private uint CreateFbo(uint tex)
    {
        var f = _gl.GenFramebuffer();
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, f);
        _gl.FramebufferTexture2D(FramebufferTarget.Framebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, tex, 0);
        return f;
    }

    public void BindDraw()
    {
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        _gl.Viewport(0, 0, (uint)GlVram.Width, (uint)GlVram.Height);
    }

    public void BindSampleDraw()
    {
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _gpuFbo);
        _gl.Viewport(0, 0, (uint)VramShadow.Width, (uint)VramShadow.Height);
    }

    public uint BeginSampleRead()
    {
        Copy1x(_sampleTex, _gpuTex, 0, 0, VramShadow.Width, VramShadow.Height, 0, 0);
        return _sampleTex;
    }

    public void CommitDraw(int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        Copy1x(_gpuTex, _sampleTex, x, y, w, h, x, y);
    }

    public uint BeginDestRead(uint targetTex, int targetW, int targetH, int x, int y, int w, int h)
    {
        _gl.TextureBarrier();
        return targetTex;
    }

    public void SampleBarrier()
    {
        _gl.TextureBarrier();
    }

    public void WriteRect(int x, int y, int w, int h, ReadOnlySpan<ushort> px)
    {
        // 0054. Sample texture is never an FBO attachment, so this is a buffer
        // upload rather than a render-target write. Sampling it from Flush does
        // not wait on a blit.
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
        _gl.BindTexture(TextureTarget.Texture2D, _sampleTex);
        _gl.PixelStore(PixelStoreParameter.UnpackAlignment, 2);
        _gl.TexSubImage2D(TextureTarget.Texture2D, 0, x, y, (uint)w, (uint)h,
            PixelFormat.Rgba, PixelType.UnsignedShort1555Rev, px);
    }

    public void Promote(int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        int s = GlVram.Scale;
        BlitFromSample(x, y, w, h, _fbo, x * s, y * s, w * s, h * s);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
    }

    public void Publish(int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0) return;
        int s = GlVram.Scale;
        _gl.Disable(EnableCap.ScissorTest);
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _fbo);
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, _gpuFbo);
        _gl.BlitFramebuffer(x * s, y * s, (x + w) * s, (y + h) * s,
            x, y, x + w, y + h, ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Nearest);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        CommitDraw(x, y, w, h);
    }

    public void BlitSample(int sx, int sy, int sw, int sh, uint dstFbo, int dstW, int dstH, int dx, int dy, int dw, int dh)
    {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        BlitFromSample(sx, sy, sw, sh, dstFbo, dx, dy, dw, dh);
        _ = dstW;
        _ = dstH;
    }

    public void Fill(int x, int y, int w, int h, ushort color15)
    {
        float r = (color15 & 0x1F) / 31f, g = ((color15 >> 5) & 0x1F) / 31f, b = ((color15 >> 10) & 0x1F) / 31f;
        var a = (color15 & 0x8000) != 0 ? 1f : 0f;
        _gl.Enable(EnableCap.ScissorTest);
        _gl.ClearColor(r, g, b, a);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        _gl.Scissor(x * GlVram.Scale, y * GlVram.Scale, (uint)Math.Max(0, w * GlVram.Scale),
            (uint)Math.Max(0, h * GlVram.Scale));
        _gl.Clear(ClearBufferMask.ColorBufferBit);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _gpuFbo);
        _gl.Viewport(0, 0, (uint)VramShadow.Width, (uint)VramShadow.Height);
        _gl.Scissor(x, y, (uint)Math.Max(0, w), (uint)Math.Max(0, h));
        _gl.Clear(ClearBufferMask.ColorBufferBit);
        _gl.Disable(EnableCap.ScissorTest);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
        _gl.Viewport(0, 0, (uint)GlVram.Width, (uint)GlVram.Height);
        CommitDraw(x, y, w, h);
    }

    public void CopyRect(int sx, int sy, int dx, int dy, int w, int h)
    {
        EnsureScratch();
        Copy1x(_sampleTex, _scratchTex, sx, sy, w, h, 0, 0);
        Copy1x(_scratchTex, _sampleTex, 0, 0, w, h, dx, dy);
        Copy1x(_gpuTex, _scratchTex, sx, sy, w, h, 0, 0);
        Copy1x(_scratchTex, _gpuTex, 0, 0, w, h, dx, dy);

        int sw = w * GlVram.Scale, sh = h * GlVram.Scale;
        var overlap = sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h;
        if (!overlap)
        {
            _gl.CopyImageSubData(_tex, CopyImageSubDataTarget.Texture2D, 0, sx * GlVram.Scale, sy * GlVram.Scale, 0,
                _tex, CopyImageSubDataTarget.Texture2D, 0, dx * GlVram.Scale, dy * GlVram.Scale, 0, (uint)sw, (uint)sh,
                1);
            return;
        }

        _gl.CopyImageSubData(_tex, CopyImageSubDataTarget.Texture2D, 0, sx * GlVram.Scale, sy * GlVram.Scale, 0,
            _scratchTex, CopyImageSubDataTarget.Texture2D, 0, 0, 0, 0, (uint)sw, (uint)sh, 1);
        _gl.CopyImageSubData(_scratchTex, CopyImageSubDataTarget.Texture2D, 0, 0, 0, 0,
            _tex, CopyImageSubDataTarget.Texture2D, 0, dx * GlVram.Scale, dy * GlVram.Scale, 0, (uint)sw, (uint)sh, 1);
    }

    public void ReadRect(int x, int y, int w, int h, Span<ushort> dst)
    {
        _gl.Disable(EnableCap.ScissorTest);
        BindSampleRead();
        _gl.PixelStore(PixelStoreParameter.PackAlignment, 2);
        _gl.ReadPixels(x, y, (uint)w, (uint)h, PixelFormat.Rgba, PixelType.UnsignedShort1555Rev, dst);
        UnbindSampleRead();
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, _fbo);
    }

    private void Copy1x(uint src, uint dst, int sx, int sy, int w, int h, int dx, int dy)
    {
        if (w <= 0 || h <= 0) return;
        _gl.CopyImageSubData(src, CopyImageSubDataTarget.Texture2D, 0, sx, sy, 0,
            dst, CopyImageSubDataTarget.Texture2D, 0, dx, dy, 0, (uint)w, (uint)h, 1);
    }

    private void BindSampleRead()
    {
        _gl.BindFramebuffer(FramebufferTarget.ReadFramebuffer, _readFbo);
        _gl.FramebufferTexture2D(FramebufferTarget.ReadFramebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, _sampleTex, 0);
    }

    private void UnbindSampleRead()
    {
        _gl.FramebufferTexture2D(FramebufferTarget.ReadFramebuffer, FramebufferAttachment.ColorAttachment0,
            TextureTarget.Texture2D, 0, 0);
        _gl.BindFramebuffer(FramebufferTarget.Framebuffer, 0);
    }

    private void BlitFromSample(int sx, int sy, int sw, int sh, uint dstFbo, int dx, int dy, int dw, int dh)
    {
        _gl.Disable(EnableCap.ScissorTest);
        BindSampleRead();
        _gl.BindFramebuffer(FramebufferTarget.DrawFramebuffer, dstFbo);
        _gl.BlitFramebuffer(sx, sy, sx + sw, sy + sh, dx, dy, dx + dw, dy + dh,
            ClearBufferMask.ColorBufferBit, BlitFramebufferFilter.Nearest);
        UnbindSampleRead();
    }

    private void EnsureScratch()
    {
        if (_scratchTex != 0) return;
        _scratchTex = CreateTex(GlVram.Width, GlVram.Height);
    }

    public void Dispose()
    {
        if (_fbo != 0) _gl.DeleteFramebuffer(_fbo);
        if (_gpuFbo != 0) _gl.DeleteFramebuffer(_gpuFbo);
        if (_readFbo != 0) _gl.DeleteFramebuffer(_readFbo);
        if (_tex != 0) _gl.DeleteTexture(_tex);
        if (_gpuTex != 0) _gl.DeleteTexture(_gpuTex);
        if (_sampleTex != 0) _gl.DeleteTexture(_sampleTex);
        if (_scratchTex != 0) _gl.DeleteTexture(_scratchTex);
    }
}
