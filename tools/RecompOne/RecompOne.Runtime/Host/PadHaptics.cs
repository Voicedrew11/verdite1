using System.Reflection;
using Silk.NET.SDL;
using RecompOne.Runtime.Hardware;

namespace RecompOne.Runtime.Host;

/// <summary>
/// A DualSense's haptics over USB (0105): the pad is a USB sound card with four
/// output channels, the first two its speaker and headset and the last two its left
/// and right voice-coil actuators, so a wave played into channels 3 and 4 is felt
/// rather than heard. <see cref="InputManager"/> drives it under its rumble lock in
/// place of SDL's two-motor rumble whenever <see cref="Controller.WantHdRumble"/>
/// holds and pad 1 is a DualSense whose sound card is there (over Bluetooth it is
/// not).
///
/// The stream is opened under its own application name, <c>&lt;assembly&gt; haptics</c>,
/// so a mixer's or WirePlumber's saved mute for the game's sound does not apply to it.
///
/// SDL's DualSense driver turns the audio haptics off (its <c>0x02</c> enable bit)
/// whenever it is asked for non-zero rumble, and a zero-rumble report leaves the bit
/// clear again; so the motors are stopped once when this opens and never asked for
/// anything while it is open.
///
/// The wave is synthesised here, both bands summed as sines with their phases kept
/// across blocks and their amplitudes ramped over each block, and queued about
/// <see cref="LeadMs"/> ahead of the device (SDL's queue, no callback).
/// </summary>
internal static unsafe class PadHaptics
{
    private const int Rate = 48000;
    private const int Channels = 4;
    private const ushort AudioF32 = 0x8120;            // AUDIO_F32LSB
    private const int AllowChannelsChange = 0x04;      // SDL_AUDIO_ALLOW_CHANNELS_CHANGE

    /// <summary>How far ahead of the device the wave is queued.</summary>
    private const int LeadMs = 40;

    private static uint _dev;
    private static bool _audioInit;
    private static double _phaseLow, _phaseHigh;
    private static float _low, _high;
    private static float[] _block = [];

    public static bool Open => _dev != 0;

    /// <summary>Find and open pad 1's sound card; true when it is open with four channels.</summary>
    public static bool TryOpen(Sdl sdl, GameController* pad)
    {
        if (_dev != 0) return true;
        if (sdl.GameControllerGetType(pad) != GameControllerType.PS5) return false;

        if (!_audioInit)
        {
            if (sdl.InitSubSystem(Sdl.InitAudio) != 0)
            {
                Console.WriteLine($"[Input] haptics: no SDL audio ({sdl.GetErrorS()}); two-motor rumble instead");
                return false;
            }
            _audioInit = true;
        }

        string? name = null;
        var count = sdl.GetNumAudioDevices(0);
        for (var i = 0; i < count; i++)
        {
            var n = sdl.GetAudioDeviceNameS(i, 0);
            if (n != null && (n.Contains("DualSense", StringComparison.OrdinalIgnoreCase) ||
                              n.Contains("Wireless Controller", StringComparison.OrdinalIgnoreCase)))
            {
                name = n;
                break;
            }
        }
        if (name == null)
        {
            Console.WriteLine($"[Input] haptics: no DualSense sound card among {count} outputs " +
                              $"({sdl.GetCurrentAudioDriverS()}); it is there over USB only; two-motor rumble instead");
            return false;
        }

        AudioSpec want = default, got = default;
        want.Freq = Rate;
        want.Format = AudioF32;
        want.Channels = Channels;
        want.Samples = 512;
        // Its own name: under the process's (dotnet) the game's sound and its saved mute
        // are shared. SDL2 reads the first hint; SDL3 under sdl2-compat reads the second,
        // which names the whole program, so it is put back once the stream is open.
        var app = $"{Assembly.GetEntryAssembly()?.GetName().Name ?? "Game"} haptics";
        sdl.SetHint("SDL_AUDIO_DEVICE_APP_NAME", app);
        sdl.SetHint("SDL_APP_NAME", app);
        sdl.SetHint("SDL_AUDIO_DEVICE_STREAM_NAME", "Haptics");
        _dev = sdl.OpenAudioDevice(name, 0, &want, &got, AllowChannelsChange);
        sdl.ResetHint("SDL_APP_NAME");
        if (_dev == 0)
        {
            Console.WriteLine($"[Input] haptics: could not open \"{name}\" ({sdl.GetErrorS()}); two-motor rumble instead");
            return false;
        }
        if (got.Channels != Channels)
        {
            Console.WriteLine($"[Input] haptics: \"{name}\" opened with {got.Channels} channels, not 4 " +
                              "(set its profile to four channels); two-motor rumble instead");
            Close(sdl);
            return false;
        }

        _phaseLow = _phaseHigh = 0;
        _low = _high = 0f;
        sdl.GameControllerRumble(pad, 0, 0, 0);     // SDL sends nothing for zero: a no-op on a fresh pad
        sdl.PauseAudioDevice(_dev, 0);
        Console.WriteLine($"[Input] haptics: \"{name}\", {got.Freq} Hz, 4 channels ({sdl.GetCurrentAudioDriverS()})");
        return true;
    }

    public static void Close(Sdl sdl)
    {
        if (_dev == 0) return;
        sdl.ClearQueuedAudio(_dev);
        sdl.CloseAudioDevice(_dev);
        _dev = 0;
    }

    /// <summary>Stop at once: what is queued is dropped.</summary>
    public static void Stop(Sdl sdl)
    {
        if (_dev == 0) return;
        sdl.ClearQueuedAudio(_dev);
        _low = _high = 0f;
    }

    /// <summary>Keep the queue <see cref="LeadMs"/> ahead with <paramref name="wave"/>.</summary>
    public static void Feed(Sdl sdl, Controller.RumbleWave wave)
    {
        if (_dev == 0) return;
        var frameBytes = Channels * sizeof(float);
        var queued = (int)(sdl.GetQueuedAudioSize(_dev) / (uint)frameBytes);
        var frames = Rate * LeadMs / 1000 - queued;
        if (frames <= 0) return;

        var n = frames * Channels;
        if (_block.Length < n) _block = new float[n];

        float low = Math.Clamp(wave.Low, 0f, 1f), high = Math.Clamp(wave.High, 0f, 1f);
        double stepLow = 2 * Math.PI * Math.Clamp(wave.LowHz, 20f, 1000f) / Rate;
        double stepHigh = 2 * Math.PI * Math.Clamp(wave.HighHz, 20f, 1000f) / Rate;
        for (var i = 0; i < frames; i++)
        {
            var k = (i + 1) / (float)frames;
            var a = _low + (low - _low) * k;
            var b = _high + (high - _high) * k;
            var s = 0.5f * (a * (float)Math.Sin(_phaseLow) + b * (float)Math.Sin(_phaseHigh));
            _phaseLow += stepLow;
            _phaseHigh += stepHigh;
            var o = i * Channels;
            _block[o] = _block[o + 1] = 0f;
            _block[o + 2] = _block[o + 3] = s;
        }
        _phaseLow %= 2 * Math.PI;
        _phaseHigh %= 2 * Math.PI;
        _low = low;
        _high = high;

        fixed (float* p = _block) sdl.QueueAudio(_dev, p, (uint)(frames * frameBytes));
    }

    public static void Shutdown(Sdl? sdl)
    {
        if (sdl == null) return;
        Close(sdl);
        if (_audioInit) sdl.QuitSubSystem(Sdl.InitAudio);
        _audioInit = false;
    }
}
