using System.Runtime.InteropServices;
using Silk.NET.OpenAL;
using ALDevice = Silk.NET.OpenAL.Device;
using ALCtx = Silk.NET.OpenAL.Context;

namespace RecompOne.Runtime.Host;

public static class AudioStats
{
    public static long Underruns, Stalls;
    public static int DeviceRate;
    public static string Resampler = "default";
}

internal static unsafe class Audio
{
    private static ALContext? _alc;
    private static AL? _al;
    private static ALDevice* _device;
    private static ALCtx* _context;


    private const int NumBuffers = 8;
    private const int FramesPerBuffer = 256;

    private static uint _source;
    private static uint[] _buffers = new uint[NumBuffers];
    private static short[] _sampleBuf = new short[FramesPerBuffer * 2];

    private static Thread? _mixerThread;
    private static Spu? _spu;
    private static volatile bool _running;
    private static float _masterVolume = 1.0f;

    public static void Initialize()
    {
        try
        {
            _alc = ALContext.GetApi(true);
            _al = AL.GetApi(true);
            _device = _alc.OpenDevice("");
            if (_device == null)
            {
                Console.Error.WriteLine("[Host] no audio device, audio disabled");
                return;
            }

            _context = _alc.CreateContext(_device, null);
            _alc.MakeContextCurrent(_context);

            _source = _al.GenSource();
            _al.SetSourceProperty(_source, SourceFloat.Gain, _masterVolume);
            ReadDeviceRate();
            ChooseResampler();
            fixed (uint* ptr = _buffers)
            {
                _al.GenBuffers(NumBuffers, ptr);
            }

            //initial empty rihgt
            for (var i = 0; i < _buffers.Length; i++)
            {
                _al.BufferData(_buffers[i], BufferFormat.Stereo16, _sampleBuf, 44100);
                var b = _buffers[i];
                _al.SourceQueueBuffers(_source, 1, &b);
            }

            _al.SourcePlay(_source);

            _running = true;
            _mixerThread = new Thread(MixerLoop) { IsBackground = true, Name = "spu-mixer" };
            _mixerThread.Start();
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"[Host] audio init failed: {e.Message}");
        }
    }

    public static void Attach(Spu? spu)
    {
        if (spu == null) return;
        _spu = spu;
        spu.VoiceGain = Config.ConfigManager.Game.SpuVolume;
        spu.XaGain = Config.ConfigManager.Game.XaVolume;
    }

    public static void Detach()
    {
        _spu = null;
    }

    public static void SetMasterVolume(float volume)
    {
        _masterVolume = Math.Clamp(volume, 0f, 1f);
        if (_al != null && _source != 0)
            _al.SetSourceProperty(_source, SourceFloat.Gain, _masterVolume);
    }

    private const int AlcFrequency = 0x1007;
    private const int AlNumResamplersSoft = 0x1210;
    private const int AlSourceResamplerSoft = 0x1212;
    private const int AlResamplerNameSoft = 0x1213;

    private static void ReadDeviceRate()
    {
        var rate = 0;
        _alc!.GetContextProperty(_device, (GetContextInteger)AlcFrequency, 1, &rate);
        AudioStats.DeviceRate = rate;
    }

    // AL_SOFT_source_resampler lists its resamplers in rising quality, so the last one is the best on offer.
    private static void ChooseResampler()
    {
        if (!_al!.IsExtensionPresent("AL_SOFT_source_resampler")) return;
        var count = _al.GetStateProperty((StateInteger)AlNumResamplersSoft);
        if (count <= 0) return;
        _al.SetSourceProperty(_source, (SourceInteger)AlSourceResamplerSoft, count - 1);
        AudioStats.Resampler = ResamplerName(count - 1) ?? $"#{count - 1}";
    }

    // Silk binds no alGetStringiSOFT; only the copy of OpenAL holding our context answers, the others return null.
    private static string? ResamplerName(int index)
    {
        foreach (var name in new[] { "libopenal.so", "libopenal.so.1", "soft_oal.dll", "openal32.dll", "libopenal.dylib" })
        {
            if (!NativeLibrary.TryLoad(name, typeof(AL).Assembly, null, out var lib)) continue;
            if (!NativeLibrary.TryGetExport(lib, "alGetStringiSOFT", out var fn)) continue;
            var text = Marshal.PtrToStringUTF8((IntPtr)((delegate* unmanaged<int, int, byte*>)fn)(AlResamplerNameSoft, index));
            if (text != null) return text;
        }

        return null;
    }

    private static readonly int BufferMs = Math.Max(1, FramesPerBuffer * 1000 / 44100);

    private static void MixerLoop()
    {
        while (_running)
        {
            var spu = _spu;
            if (spu != null) FillBuffers(spu);
            Thread.Sleep(spu != null ? BufferMs : 20);
        }
    }

    private static void FillBuffers(Spu spu)
    {
        _al!.GetSourceProperty(_source, GetSourceInteger.BuffersProcessed, out var processed);
        if (processed >= NumBuffers) AudioStats.Underruns++;
        while (processed > 0)
        {
            uint buf = 0;
            _al.SourceUnqueueBuffers(_source, 1, &buf);

            spu.Mix(_sampleBuf, FramesPerBuffer);

            _al.BufferData(buf, BufferFormat.Stereo16, _sampleBuf, 44100);
            _al.SourceQueueBuffers(_source, 1, &buf);
            processed--;
        }

        _al.GetSourceProperty(_source, GetSourceInteger.SourceState, out var state);
        if (state != (int)SourceState.Playing)
        {
            AudioStats.Stalls++;
            _al.SourcePlay(_source);
        }
    }

    public static void Shutdown()
    {
        if (_alc == null) return;
        _running = false;
        _mixerThread?.Join();
        if (_al != null)
        {
            _al.SourceStop(_source);
            _al.DeleteSource(_source);
            _al.DeleteBuffers(_buffers);
        }

        if (_context != null) _alc.DestroyContext(_context);
        if (_device != null) _alc.CloseDevice(_device);
    }
}