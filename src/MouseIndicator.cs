using System.Numerics;
using ImGuiNET;
using RecompOne.Runtime.Host.Window;

namespace Verdite.Core;

/// <summary>
/// What tells the player the pointer has been captured or given back: a small
/// white pixel-art mouse in the top right of the game picture, faded in when the
/// state changes and faded out again a moment later.
///
/// **It replaces a toast, and the toast was the bug.** Capturing and releasing
/// the pointer is something a player does *while playing* — it is how you get to
/// the menu bar and back — so it happens often, and a titled notification card
/// sliding in over the game every time is a modal-looking interruption reporting
/// a state the player just asked for. The state is a boolean and the change is
/// the only interesting moment, so the whole of it fits in one glyph that
/// announces itself and then leaves.
///
/// **The glyph carries the state and the fade carries the change.** Captured is
/// a solid mouse; released is the same silhouette with a diagonal cut through it,
/// which is the universal "off" and so needs no learning. Both are drawn as pixel
/// blocks rather than as a font glyph or a texture, at an integer number of
/// screen pixels a cell, so they stay crisp at any interface scale and read as
/// part of a 1996 game rather than as desktop chrome.
///
/// It is anchored to the rectangle <see cref="Picture"/> returns — the game
/// picture, not the window — so an overlay sits over the game's own image and not
/// over the port's menu bar or the letterbox bar beside a 4:3 picture.
///
/// Draws nothing at all while faded out, takes no input (<c>NoInputs</c>) and
/// writes no game memory.
///
/// **Never judged by eye**: whether the top-right corner is where the eye is,
/// whether 1.75 s is long enough to notice and short enough not to nag, and
/// whether the cut reads as "released" over a bright scene.
/// </summary>
public sealed class MouseIndicator : IFloatingPanel
{
    public static readonly MouseIndicator Instance = new();
    MouseIndicator() { }

    /// <summary>
    /// The game picture's rectangle in screen space, or null when there is none.
    /// Defaults to <c>OutputView</c>; a game with a rectangle of its own sets it.
    /// </summary>
    public static Func<(Vector2 Min, Vector2 Max)?> Picture { get; set; } = () =>
        OutputView.Valid ? (OutputView.Min, OutputView.Max) : null;

    public string Name => $"{Game.Id}mouseind";

    /// <summary>Always drawable, never persisted: the fade is the open state, and
    /// <c>Draw</c> returns before it begins a window when there is nothing
    /// to show. A setter that wrote anything would give "Reset view" an opinion
    /// about a transient.</summary>
    public bool IsOpen { get => true; set { } }

    /// <summary>Fade in, hold, fade out — milliseconds. The hold is what makes it
    /// an announcement rather than a HUD element: a permanent icon is one more
    /// thing on screen for a state the player is already holding in their head.
    /// </summary>
    const long FadeInMs = 140, HoldMs = 1100, FadeOutMs = 420;

    static long _shown = long.MinValue;
    static bool _captured;

    /// <summary>
    /// Silence the glyph outright. A cinematic camera or a flythrough being
    /// filmed sets this so capture announcements do not fade in over the
    /// picture. Both ends are gated -- Show so nothing queues underneath, Draw so
    /// switching on mid-fade hides it at once.
    /// </summary>
    public static bool Suppressed { get; set; }

    /// <summary>Announce the state. Called from <c>Mouse.SetCaptured</c>
    /// and from the once-a-session hint in <c>Mouse.TakeLook</c>; a repeat
    /// restarts the fade, so mashing the capture key keeps it visible rather than
    /// stacking anything up.</summary>
    public static void Show(bool captured)
    {
        if (Suppressed) return;
        _captured = captured;
        _shown = Environment.TickCount64;
    }

    static float Alpha()
    {
        long t = Environment.TickCount64 - _shown;
        if (t < 0 || t >= FadeInMs + HoldMs + FadeOutMs) return 0f;
        if (t < FadeInMs) return t / (float)FadeInMs;
        t -= FadeInMs;
        if (t < HoldMs) return 1f;
        return 1f - (t - HoldMs) / (float)FadeOutMs;
    }

    /// <summary>
    /// The mouse, nine cells across and thirteen down: two cells of cable, a
    /// rounded shell, and the seam between the two buttons with the wheel in it.
    /// Authored rather than generated, because a shape this small is decided one
    /// cell at a time.
    /// </summary>
    static readonly string[] Glyph =
    [
        "....#....",
        "....#....",
        "...###...",
        "..#####..",
        ".##.#.##.",
        ".##.#.##.",
        ".#######.",
        ".#######.",
        ".#######.",
        ".#######.",
        ".#######.",
        "..#####..",
        "...###...",
    ];

    /// <summary>The released state cuts a two-cell diagonal out of the same
    /// silhouette. Computed from the glyph rather than authored twice, so the two
    /// cannot drift apart.
    ///
    /// Two cells wide and one row a column, which is the only slope that reads at
    /// this size. The offset puts it under the buttons and out at the bottom right
    /// corner, so the top of the shell and the cable survive and the thing is
    /// still a mouse.</summary>
    static bool Cut(int x, int y) => y - x is 4 or 5;

    public void Draw()
    {
        if (Suppressed) return;
        float a = Alpha();
        if (a <= 0.001f) return;

        var picture = Picture();
        if (picture is not { } r) return;
        var size = r.Max - r.Min;
        if (size.X < 64f || size.Y < 64f) return;

        // A cell is a whole number of screen pixels or the shell's one-cell
        // outline lands on half a pixel and the whole thing softens. Sized off
        // the picture rather than off Theme.Scale, since it belongs to the game's
        // image and not to the port's chrome.
        float cell = MathF.Max(2f, MathF.Round(size.Y / 180f));
        float w = cell * Glyph[0].Length, h = cell * Glyph.Length;
        float margin = MathF.Max(cell * 2f, MathF.Round(size.Y * 0.03f));

        ImGui.SetNextWindowPos(r.Min);
        ImGui.SetNextWindowSize(size);
        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);

        // NoBringToFrontOnFocus is absent for the reason the map overlay records:
        // a window carrying it is created at the front of g.Windows, which is the
        // back of the display order, and would be drawn underneath the dockspace's
        // opaque background.
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags.NoDecoration | ImGuiWindowFlags.NoInputs |
            ImGuiWindowFlags.NoNav | ImGuiWindowFlags.NoFocusOnAppearing |
            ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoDocking |
            ImGuiWindowFlags.NoBackground;

        if (ImGui.Begin($"##{Game.Id}mouseind", flags))
        {
            var p = ImGui.GetWindowPos();
            var o = new Vector2(p.X + size.X - margin - w, p.Y + margin);
            var dl = ImGui.GetWindowDrawList();

            // A one-cell shadow under the white, because the picture behind it is
            // whatever the dungeon happens to be: a torch-lit wall is bright
            // enough to lose a white shell, and the cut only reads as a cut if
            // something separates the two halves.
            uint ink = ImGui.GetColorU32(new Vector4(0f, 0f, 0f, 0.55f * a));
            uint white = ImGui.GetColorU32(new Vector4(1f, 1f, 1f, a));

            Blit(dl, o + new Vector2(cell, cell), cell, ink);
            Blit(dl, o, cell, white);
        }

        ImGui.End();
        ImGui.PopStyleVar();
    }

    static void Blit(ImDrawListPtr dl, Vector2 o, float cell, uint colour)
    {
        for (int y = 0; y < Glyph.Length; y++)
        for (int x = 0; x < Glyph[y].Length; x++)
        {
            if (Glyph[y][x] != '#') continue;
            if (!_captured && Cut(x, y)) continue;

            var q0 = new Vector2(o.X + x * cell, o.Y + y * cell);
            dl.AddRectFilled(q0, q0 + new Vector2(cell, cell), colour);
        }
    }
}
