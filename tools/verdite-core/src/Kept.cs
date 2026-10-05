namespace Verdite.Core;

/// <summary>
/// The two ways a port reads a setting: the environment variable first, then the
/// value kept in interface.ini. A variable set wins over the saved key, which is
/// read once the config has loaded. The set of variable-set keys is passed in,
/// so two callers over the same store keep separate records of it.
/// </summary>
public static class Kept
{
    /// <summary>
    /// Store booleans as ints (<c>0</c>/<c>1</c>) rather than as the view's own
    /// bool (<c>"True"</c>/<c>"False"</c>). A game whose settings page writes
    /// ints sets this once, before anything reads.
    /// </summary>
    public static bool BoolsAsInts;

    public static void Env(string name, string key, ref bool value, HashSet<string> from)
    {
        string? v = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(v)) return;
        value = v.Trim().ToLowerInvariant() is "1" or "on" or "true" or "yes";
        from.Add(key);
    }

    public static void Env(string name, string key, ref float value, HashSet<string> from)
    {
        string? v = Environment.GetEnvironmentVariable(name);
        if (string.IsNullOrWhiteSpace(v)) return;
        if (!float.TryParse(v, System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out float f))
            throw new ArgumentException($"{name}: cannot read '{v}'");
        value = f;
        from.Add(key);
    }

    public static void Saved(string key, ref bool value, HashSet<string> from)
    {
        if (from.Contains(key)) return;
        var view = RecompOne.Runtime.Runtime.View;
        value = BoolsAsInts ? view.GetInt(key, value ? 1 : 0) != 0 : view.GetBool(key, value);
    }

    public static void Saved(string key, ref float value, HashSet<string> from)
    {
        if (!from.Contains(key)) value = RecompOne.Runtime.Runtime.View.GetFloat(key, value);
    }

    public static void Saved(string key, ref int value, HashSet<string> from)
    {
        if (!from.Contains(key)) value = RecompOne.Runtime.Runtime.View.GetInt(key, value);
    }
}
