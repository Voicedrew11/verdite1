using System.Numerics;
using ImGuiNET;

namespace RecompOne.Runtime.Host.Window;

/// <summary>
/// Where the game picture actually landed on screen this frame, in the same
/// coordinates an ImGui draw list works in.
///
/// The picture is not the window: it is an <c>Image</c> inside the Output panel,
/// fitted to that panel's content region at the display's aspect and centred in
/// it, so a 16:9 window showing a 4:3 game leaves a bar either side, and the menu
/// bar, the dockspace and any docked panel take their share off the top and the
/// edges. Nothing outside this file knew that rectangle, so an overlay drawn over
/// the game could only anchor itself to the viewport and would sit partly over
/// the surrounding chrome.
///
/// <see cref="Valid"/> is false whenever the panel drew no picture this frame
/// (collapsed, or before the first frame is presented), so a caller can fall back
/// to the viewport rather than to a stale or empty rectangle.
/// </summary>
public static class OutputView
{
    public static bool Valid { get; internal set; }
    public static Vector2 Min { get; internal set; }
    public static Vector2 Max { get; internal set; }
    public static Vector2 Size => Max - Min;

    /// <summary>
    /// The pointer is over the picture and nothing is in front of it. The picture
    /// is an ImGui window like any other, so <c>io.WantCaptureMouse</c> is true
    /// over it and cannot tell a click on the game from a click on a panel.
    /// </summary>
    public static bool Hovered { get; internal set; }

    /// <summary>
    /// The pointer is over some other ImGui window than this panel -- a floating
    /// panel in front of the picture, say. <see cref="Hovered"/> false cannot say
    /// that: it is also false while another item is active or a popup blocks, so
    /// a caller testing a click against the rectangle needs this to tell a click
    /// on a panel from a click on the game.
    /// </summary>
    public static bool Covered { get; internal set; }

    /// <summary>
    /// The display buffer the *game* programmed, in its own pixels -- 320x240
    /// here. Published because <see cref="Min"/> and <see cref="Max"/> are a
    /// rectangle and not a scale, and the inverse of that rectangle is what an
    /// overlay needs to ask which thing the game drew is under the pointer.
    ///
    /// **Deliberately the GPU's numbers and not the render target's.** The GL
    /// backend hands <c>SetTexture</c> a target sized by the render-scale
    /// setting, and a 960x720 target is still a 320x240 picture as far as the
    /// game's own coordinates are concerned. It is also the game's *own* width,
    /// so a port rendering a widescreen margin either side presents something
    /// wider than <see cref="GameW"/> -- the height is the axis that stays
    /// exact. Zero until the first frame is presented.
    /// </summary>
    public static int GameW { get; internal set; }
    public static int GameH { get; internal set; }

    /// <summary>
    /// The dock node the Output panel sits in, or 0 while it floats. A node that
    /// holds a window is a leaf, so a port can split it to dock a panel of its own
    /// beside the picture.
    /// </summary>
    public static uint DockId { get; internal set; }
}

internal sealed class OutputPanel : IPanel
{
    public string Name => "Output";
    public string TitleKey => "panel.output";

    public bool IsOpen
    {
        get => true;
        set { }
    }

    private static uint _texId;
    private static int _texW, _texH;
    private static float _aspect = 4f / 3f;

    public static bool IsDocked { get; private set; }

    public static void SetTexture(uint id, int w, int h, float aspect = 0f)
    {
        (_texId, _texW, _texH, _aspect) = (id, w, h, aspect > 0f ? aspect : 4f / 3f);
    }

    //idea: in the future make this be able to draw images so you can have ornamented backgrounds
    public void Draw()
    {
        ImGui.SetNextWindowSize(new Vector2(640, 480), ImGuiCond.FirstUseEver);
        ImGui.PushStyleColor(ImGuiCol.WindowBg, new Vector4(0f, 0f, 0f, 1f));

        //The picture is the point of this panel, so it gets none of the chrome
        //every other panel wants. The themed WindowPadding (12,10) and the 1px
        //WindowBorderSize are read by Begin when it computes the inner rect, so
        //they are pushed around Begin only and popped straight after it: the
        //toasts drawn below still lay themselves out on the real style, and no
        //other panel is affected. Without this a docked, tab-bar-less Output
        //panel filling the dockspace still letterboxes the game behind a band of
        //window background on all four sides -- scaled by Theme.Scale, so it is
        //widest exactly where the DPI is misread highest.
        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0f);
        var visible = ImGui.Begin(this.Title());
        ImGui.PopStyleVar(2);
        //Asked inside Begin, where "this window" is the Output panel: ImGui's
        //hovered window is someone else's exactly when another window sits under
        //the pointer, whatever is active or whether the picture was drawn.
        OutputView.Covered =
            ImGui.IsWindowHovered(ImGuiHoveredFlags.AnyWindow | ImGuiHoveredFlags.AllowWhenBlockedByActiveItem | ImGuiHoveredFlags.AllowWhenBlockedByPopup) &&
            !ImGui.IsWindowHovered(ImGuiHoveredFlags.RootAndChildWindows | ImGuiHoveredFlags.AllowWhenBlockedByActiveItem | ImGuiHoveredFlags.AllowWhenBlockedByPopup);
        IsDocked = ImGui.IsWindowDocked();
        OutputView.DockId = IsDocked ? ImGui.GetWindowDockID() : 0u;
        OutputView.Valid = false;
        OutputView.Hovered = false;

        if (!visible)
        {
            ImGui.End();
            ImGui.PopStyleColor();
            return;
        }

        if (_texId != 0 && _texW > 0 && _texH > 0)
        {
            var avail = ImGui.GetContentRegionAvail();
            var imageSize = FitAspect(new Vector2(_aspect, 1f), avail);
            var offset = (avail - imageSize) * 0.5f;
            ImGui.SetCursorPos(ImGui.GetCursorPos() + offset);

            //Published from here rather than computed from the window: this is the
            //one point that knows both where the image starts on screen and how
            //big the aspect fit made it.
            var min = ImGui.GetCursorScreenPos();
            OutputView.Min = min;
            OutputView.Max = min + imageSize;
            OutputView.Valid = imageSize.X > 0f && imageSize.Y > 0f;

            ImGui.Image((nint)_texId, imageSize);
            OutputView.Hovered = OutputView.Valid && ImGui.IsItemHovered();
        }

        ToastNotifications.Draw();

        ImGui.End();
        ImGui.PopStyleColor();
    }

    private static Vector2 FitAspect(Vector2 src, Vector2 dst)
    {
        var scale = MathF.Min(dst.X / src.X, dst.Y / src.Y);
        return src * scale;
    }
}