namespace Kf1;

/// <summary>
/// The addresses this port knows, with the names the frame profiler shows for them.
/// Verdite Core's <c>FrameProfiler</c> takes this table in <c>Configure</c>; a row
/// whose label starts "stage " is one of the main-loop stages the "stages" probe times.
/// Only functions in generated/game.cs are listed.
/// </summary>
public static class ProfilerKnown
{
    public static readonly (string Overlay, uint Addr, string Label)[] Table =
    [
        // The main loop (func_8001482C) and its gate; see patches/StageProbe.cs.
        ("game", 0x80018880, "stage A"),
        ("game", 0x80017E3C, "stage B"),
        ("game", 0x8003303C, "stage C"),
        ("game", 0x8002CAD4, "stage D"),
        ("game", 0x80030818, "stage E"),
        ("game", 0x80031CC8, "stage F"),
        ("game", 0x8003A760, "stage G"),
        ("game", 0x8003596C, "stage H"),
        ("game", 0x8001FDE4, "stage I: the renderer"),
        ("game", 0x800149F4, "stage gate: the frame gate"),

        // The renderer's callees (patches/Renderer.cs).
        ("game", 0x8001C184, "camera block"),
        ("game", 0x8001BFB8, "frame head"),
        ("game", 0x800209A8, "pose mark"),
        ("game", 0x8004CB08, "SetGeomScreen"),
        ("game", 0x8001E83C, "map walk"),
        ("game", 0x8004D7B4, "SetLightMatrix"),
        ("game", 0x8001F8B0, "compass"),
        ("game", 0x8001F9D4, "HUD sprites"),
        ("game", 0x8004D784, "SetRotMatrix"),
        ("game", 0x8001FAFC, "selected item"),
        ("game", 0x8004E9B8, "RotMatrix"),
        ("game", 0x8004D814, "SetTransMatrix"),
        ("game", 0x8001E230, "HUD model draw (FT4 writer)"),
        ("game", 0x8001F218, "model walk"),
        ("game", 0x8001F798, "arm"),
        ("game", 0x8001C050, "present"),
        ("game", 0x80020A98, "pose sweep"),

        // The model submitters, the pose blender, the vertex cache and the packet writers.
        ("game", 0x8001EBB8, "submitter"),
        ("game", 0x8001E9A4, "submitter: objects"),
        ("game", 0x8001ED90, "submitter: sprites"),
        ("game", 0x8001EEDC, "submitter: creatures"),
        ("game", 0x8001F0C4, "submitter"),
        ("game", 0x800205D4, "pose blender"),
        ("game", 0x8001C60C, "vertex cache"),
        ("game", 0x8001C7F8, "packet writer: display list"),
        ("game", 0x8001D730, "packet writer: FT objects"),
        ("game", 0x8001DE18, "packet writer: map GT"),
        ("game", 0x8001E480, "packet writer: HUD sprites"),

        // The SDK entry points the game reaches.
        ("game", 0x80050CF0, "DrawOTag"),
        ("game", 0x800555E0, "VSync"),
    ];
}
