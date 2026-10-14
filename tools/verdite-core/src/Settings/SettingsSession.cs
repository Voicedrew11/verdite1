namespace Verdite.Core;

/// <summary>
/// The game menu's settings page, without its drawing: changes shown and applied
/// live, kept only on Save. See docs/SETTINGS.md in a port.
///
/// Opening it reads every setting; moving a value applies it (unless it waits for
/// the next boot) and remembers it; moving it back to where it started forgets it.
/// Save hands the store only what is still changed. Discard, or a session never
/// closed (the game quit with the page open), writes nothing and puts back every
/// value nothing else has moved since.
///
/// Runs on the game thread: the page and the shell's <c>settings</c> verb.
/// </summary>
public sealed class SettingsSession
{
    public static SettingsSession? Current { get; private set; }

    readonly Dictionary<PortSetting, double> _before = [];
    readonly Dictionary<PortSetting, double?> _changed = [];   // null: back to the default

    /// <summary>A session over <paramref name="settings"/>: the port's list of them.</summary>
    public static SettingsSession Open(IEnumerable<PortSetting> settings)
    {
        Current?.Discard();
        var session = new SettingsSession();
        foreach (var s in settings) session._before[s] = s.Live();
        return Current = session;
    }

    public bool Dirty => _changed.Count > 0;
    public IEnumerable<PortSetting> Changed => _changed.Keys;

    /// <summary>The value the page shows: the staged one, else the live one, so a
    /// change made in the Settings window meanwhile shows too.</summary>
    public double Shown(PortSetting s) =>
        s.Join is { } join ? join(Shown) : _changed.TryGetValue(s, out var v) ? v ?? s.Default : s.Live();

    /// <summary>Staged, or for a combined row, any of its parts staged.</summary>
    public bool IsChanged(PortSetting s) => s.Parts?.Any(_changed.ContainsKey) ?? _changed.ContainsKey(s);

    /// <summary>Left (-1) or Right (+1); why not, or null when it moved.</summary>
    public string? Step(PortSetting s, int dir)
    {
        if (s.LockedBy is { } env) return $"set by {env}";
        if (!s.IsUsable) return "not usable";
        double from = Shown(s), to = s.Next(from, dir);
        if (PortSetting.Same(from, to)) return "at the end";
        Stage(s, to);
        return null;
    }

    /// <summary>Back to the default: the key is removed on Save.</summary>
    public string? Reset(PortSetting s)
    {
        if (s.LockedBy is { } env) return $"set by {env}";
        if (s.Parts is { } parts)
        {
            foreach (var p in parts) Reset(p);
            return null;
        }
        if (!s.AtBoot) s.Apply(s.Default);
        _changed[s] = null;
        return null;
    }

    void Stage(PortSetting s, double value)
    {
        if (s.Parts is { } parts)
        {
            var values = s.Split!(value, Shown);
            for (int i = 0; i < parts.Length; i++)
                Stage(parts[i], double.IsNaN(values[i]) ? _before[parts[i]] : values[i]);   // the port's PortSettings.Unchanged
            return;
        }
        if (!s.AtBoot) s.Apply(value);
        if (PortSetting.Same(value, _before[s])) _changed.Remove(s);
        else _changed[s] = PortSetting.Same(value, s.Default) ? null : value;   // the default is no key
    }

    public void Save()
    {
        SettingsStore.Submit([.. _changed.Select(c => new SettingsStore.Change(c.Key, c.Value))], "game menu");
        Close();
    }

    public void Discard()
    {
        foreach (var (s, staged) in _changed)
            if (!s.AtBoot && PortSetting.Same(s.Live(), staged ?? s.Default))
                s.Apply(_before[s]);
        Close();
    }

    void Close()
    {
        _changed.Clear();
        if (Current == this) Current = null;
    }
}
