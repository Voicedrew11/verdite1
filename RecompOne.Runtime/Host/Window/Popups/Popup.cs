using System.Numerics;
using ImGuiNET;

namespace RecompOne.Runtime.Host.Window;

public abstract class Popup
{
    protected virtual Vector2 Size => new(520f, 0f);

    protected abstract string TitleKey { get; }

    protected virtual bool Closable => true;

    protected abstract void DrawContent();

    protected virtual void OnOpened()
    {
    }

    protected virtual void OnClosed()
    {
    }

    protected internal virtual void Update()
    {
    }

    public bool IsOpen { get; private set; }

    public void Open()
    {
        if (IsOpen) return;
        IsOpen = true;
        _pendingOpen = true;
        _pendingClose = false;
        PopupManager.Push(this);
        OnOpened();
    }

    public void Close()
    {
        if (!IsOpen) return;
        IsOpen = false;
        _pendingClose = true;
        PopupManager.CloseAbove(this);
        OnClosed();
    }

    public void Toggle()
    {
        if (IsOpen) Close();
        else Open();
    }

    private string Id => _id ??= $"##popup-{GetType().FullName}";
    private string? _id;

    private bool _pendingOpen;
    private bool _pendingClose;

    internal bool Finished => !IsOpen && !_pendingClose;

    internal void Render(int stackIndex)
    {
        if (_pendingOpen)
        {
            ImGui.OpenPopup(Id);
            _pendingOpen = false;
        }

        var viewport = ImGui.GetMainViewport();
        var style = ImGui.GetStyle();

        // A popup is centred and pinned every frame, and SetNextWindowPos is what
        // sets ImGui's window_pos_set_by_api -- which is the flag that suppresses
        // ImGui's own clamp of a window into its viewport. Nothing below gives the
        // size back either: the flags are NoResize, NoMove, NoScrollbar and
        // NoScrollWithMouse. So a popup larger than the window overflows
        // symmetrically off all four edges, and what left cannot be reached at
        // all -- including, in the settings popup, the UI-scale field that is the
        // one control that would undo it.
        //
        // Size is in logical units and Theme.Scale is DpiScale * UiScale, so the
        // settings popup (780x500) outgrows a 1280x720 window at scale 1.44. That
        // is inside UiScale's own range (0.5-3), and a DpiScale misread from a
        // fractionally scaled monitor -- GLFW's Wayland path reports the integer
        // wl_output scale, so 1.15 arrives as 2 -- reaches it on its own at a
        // UiScale of 1, with no way back short of editing interface.ini by hand.
        //
        // Clamping the size to the viewport is what makes that impossible rather
        // than unlikely: a scale too large for the window now costs scrolling
        // instead of costing the controls. The ##body child scrolls by default
        // (the NoScrollbar flags are this window's, not its), so a fixed-height
        // popup that had to be cut short stays whole. An auto-height one
        // (Size.Y == 0) keeps its height ImGui's to pick and is given the same
        // ceiling as a constraint, here and again on the child, which is what
        // turns its overflow into scrolling rather than into clipping.
        var room = viewport.WorkSize - style.WindowPadding * 2f;
        var size = Vector2.Min(Size * Theme.Scale, room);
        if (Size.Y <= 0f) size.Y = 0f;

        ImGui.SetNextWindowPos(viewport.GetCenter(), ImGuiCond.Always, new Vector2(0.5f, 0.5f));
        ImGui.SetNextWindowSize(size, ImGuiCond.Always);
        if (Size.Y <= 0f) ImGui.SetNextWindowSizeConstraints(Vector2.Zero, room);

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags.NoTitleBar | ImGuiWindowFlags.NoResize | ImGuiWindowFlags.NoMove |
            ImGuiWindowFlags.NoDocking | ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoScrollbar |
            ImGuiWindowFlags.NoScrollWithMouse;

        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, Vector2.Zero);
        var visible = ImGui.BeginPopupModal(Id, flags);
        ImGui.PopStyleVar();

        if (!visible)
        {
            _pendingClose = false;
            if (IsOpen) Close();
            _pendingClose = false;
            return;
        }

        DrawTitleBar();

        var padding = style.WindowPadding;
        var body = new Vector2(0f, Size.Y > 0f ? -padding.Y : 0f);
        var childFlags = ImGuiChildFlags.AlwaysUseWindowPadding |
                         (Size.Y > 0f ? ImGuiChildFlags.None : ImGuiChildFlags.AutoResizeY);

        // An AutoResizeY child sizes itself to its content, which is how the
        // auto-height popups get their height at all -- so left alone it grows
        // straight past a window that has just been clamped, and the overflow is
        // clipped silently rather than scrolled. A max constraint is how ImGui
        // caps an auto-resizing child; below the cap nothing changes, and at it
        // the child scrolls.
        if (Size.Y <= 0f)
            ImGui.SetNextWindowSizeConstraints(Vector2.Zero,
                new Vector2(float.MaxValue, room.Y - Theme.TitleBarHeight - padding.Y * 2f));

        ImGui.PushStyleVar(ImGuiStyleVar.WindowPadding, padding);
        ImGui.PushStyleVar(ImGuiStyleVar.ChildBorderSize, 0f);
        ImGui.PushStyleColor(ImGuiCol.ChildBg, Vector4.Zero);
        var bodyVisible = ImGui.BeginChild("##body", body, childFlags, ImGuiWindowFlags.NoSavedSettings);
        ImGui.PopStyleColor();
        ImGui.PopStyleVar(2);

        if (bodyVisible) DrawContent();
        ImGui.EndChild();

        if (Closable && stackIndex == PopupManager.TopIndex && ImGui.IsKeyPressed(ImGuiKey.Escape))
            Close();

        PopupManager.DrawNested(stackIndex + 1);

        if (_pendingClose)
        {
            ImGui.CloseCurrentPopup();
            _pendingClose = false;
        }

        ImGui.EndPopup();
    }

    private void DrawTitleBar()
    {
        var style = ImGui.GetStyle();
        var draw = ImGui.GetWindowDrawList();

        var height = Theme.TitleBarHeight;
        var origin = ImGui.GetCursorScreenPos();
        var end = new Vector2(origin.X + ImGui.GetWindowWidth(), origin.Y + height);

        draw.AddRectFilled(origin, end, ImGui.ColorConvertFloat4ToU32(Theme.TitleBar),
            style.WindowRounding, ImDrawFlags.RoundCornersTop);

        var title = Localization.T(TitleKey);
        draw.AddText(new Vector2(origin.X + style.WindowPadding.X, origin.Y + (height - ImGui.GetFontSize()) * 0.5f),
            ImGui.ColorConvertFloat4ToU32(Theme.TitleBarText), title);

        if (Closable) DrawCloseButton(origin, height);

        ImGui.SetCursorScreenPos(new Vector2(origin.X, end.Y));
        ImGui.Dummy(Vector2.Zero);
    }

    private void DrawCloseButton(Vector2 origin, float height)
    {
        var style = ImGui.GetStyle();
        var margin = style.FramePadding.Y;
        var size = height - margin * 2f;
        var pos = new Vector2(origin.X + ImGui.GetWindowWidth() - size - style.WindowPadding.X * 0.5f,
            origin.Y + margin);

        ImGui.SetCursorScreenPos(pos);
        var clicked = ImGui.InvisibleButton("##close", new Vector2(size, size));
        var hovered = ImGui.IsItemHovered();

        var text = Theme.TitleBarText;
        var draw = ImGui.GetWindowDrawList();
        if (hovered)
            draw.AddRectFilled(pos, pos + new Vector2(size, size),
                ImGui.ColorConvertFloat4ToU32(text with { W = 0.18f }), style.FrameRounding);

        var inset = size * 0.32f;
        var color = ImGui.ColorConvertFloat4ToU32(text with { W = hovered ? 1f : 0.8f });
        draw.AddLine(pos + new Vector2(inset, inset), pos + new Vector2(size - inset, size - inset), color,
            1.6f * Theme.Scale);
        draw.AddLine(pos + new Vector2(size - inset, inset), pos + new Vector2(inset, size - inset), color,
            1.6f * Theme.Scale);

        if (hovered) ImGui.SetTooltip(Localization.T("common.close"));
        if (clicked) Close();
    }
}