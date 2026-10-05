namespace RecompOne.Runtime.Host.Window;

public static class SettingsRegistry
{
    private static readonly List<ISettingsSection> _sections = [];
    private static readonly Dictionary<string, List<Action>> _extensions = new(StringComparer.OrdinalIgnoreCase);
    private static bool _dirty;

    public static void Register(ISettingsSection section)
    {
        if (section == null) return;
        _sections.RemoveAll(s => s.Id == section.Id);
        _sections.Add(section);
        _dirty = true;
    }

    public static void Unregister(string id)
    {
        _sections.RemoveAll(s => s.Id == id);
    }

    public static void Extend(string sectionId, Action draw)
    {
        if (!_extensions.TryGetValue(sectionId, out var list))
        {
            list = [];
            _extensions[sectionId] = list;
        }

        list.Add(draw);
    }

    internal static IReadOnlyList<Action> GetExtensions(string sectionId)
    {
        return _extensions.TryGetValue(sectionId, out var list) ? list : [];
    }

    /// <summary>
    /// Draw whatever was registered against <paramref name="slotId"/>, from inside
    /// a section's own body.
    ///
    /// <see cref="Extend"/> appends after a section has drawn everything it has,
    /// which is the right place for a group of its own but the wrong one for an
    /// option that belongs beside an existing control -- an aspect ratio next to
    /// the render scale, say. A section that wants to offer that calls this at the
    /// point it means, with an id of its own (<c>"display.render_scale"</c>), and
    /// anything registered there draws in line with the section's own widgets.
    /// </summary>
    public static void DrawSlot(string slotId)
    {
        foreach (var draw in GetExtensions(slotId)) draw();
    }

    public static IReadOnlyList<ISettingsSection> Sections
    {
        get
        {
            if (_dirty)
            {
                _sections.Sort((a, b) => a.Order != b.Order
                    ? a.Order.CompareTo(b.Order)
                    : string.Compare(a.TitleKey, b.TitleKey, StringComparison.Ordinal));
                _dirty = false;
            }

            return _sections;
        }
    }
}