namespace Verdite.Core;

/// <summary>
/// The port settings list's own checks, run once at boot: a mistake in the list is
/// the programmer's, so it stops the boot. The port hands in its lists and the widths
/// the game's menu fits (its settings page's constants).
/// </summary>
public static class SettingsCheck
{
    public static void Validate(IEnumerable<PortSetting> listed, IEnumerable<string> pages,
        Func<string, IEnumerable<PortSetting>> onPage, IEnumerable<PortSetting> menu,
        int maxRows, int labelChars, int valueChars)
    {
        var problems = new List<string>();
        var pageList = pages.ToList();
        var menuList = menu.ToList();
        foreach (var dup in listed.GroupBy(s => s.Key, StringComparer.OrdinalIgnoreCase).Where(g => g.Count() > 1))
            problems.Add($"{dup.Key} declared {dup.Count()} times");
        foreach (var page in pageList)
        {
            if (MenuFont.Check($"{page} 9/9", labelChars) is { } why) problems.Add($"page {page}: {why}");
            if (onPage(page).Count() is var n && (n == 0 || n > maxRows)) problems.Add($"page {page}: {n} rows, 1 to {maxRows}");
        }
        foreach (var s in listed)
        {
            if (s.Page is null) continue;
            if (!menuList.Contains(s)) problems.Add($"{s.Key}: has a page but is not in Menu");
            if (!pageList.Contains(s.Page)) problems.Add($"{s.Key}: page {s.Page} is not in Pages");
            if ((s.MenuLabel is null ? "missing" : MenuFont.Check(s.MenuLabel, labelChars)) is { } why)
                problems.Add($"{s.Key}: label {s.MenuLabel}: {why}");
            foreach (double v in s.Steps)
                if (MenuFont.Check(s.MenuValue(v), valueChars) is { } bad) problems.Add($"{s.Key}: value {s.MenuValue(v)}: {bad}");
        }
        if (problems.Count > 0)
            throw new InvalidOperationException("port settings: " + string.Join("; ", problems));
    }
}
