using System.Text.Json;

namespace Verdite.Launcher.Build;

/// <summary>
/// What the game's recompiler config says the recompile reads off the disc: each
/// file an overlay is sliced from, in the config's order, and the smallest that
/// file may be -- the furthest any slice of it reaches (offset + skip + size).
///
/// Read out of the config rather than written down beside it, because the two
/// would otherwise drift silently: an area module added past the current end of
/// an archive would leave the disc check passing a disc too short for it, and the
/// recompile would slice whatever bytes happened to follow. An executable overlay
/// with no size reaches its skip, 0x800, which is its PS-X EXE header.
/// </summary>
static class ShippedConfig
{
    public static IReadOnlyList<(string File, uint Floor)> DiscFiles => _files ??= Read();

    static IReadOnlyList<(string File, uint Floor)>? _files;

    static IReadOnlyList<(string File, uint Floor)> Read()
    {
        var order = new List<string>();
        var floors = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);

        try
        {
            var json = File.ReadAllText(Path.Combine(Paths.ContentConfig, Launcher.Game.RecompilerConfig));
            using var doc = JsonDocument.Parse(json, new JsonDocumentOptions
            {
                CommentHandling = JsonCommentHandling.Skip,
                AllowTrailingCommas = true,
            });

            foreach (var overlay in doc.RootElement.GetProperty("overlays").EnumerateArray())
            {
                if (!overlay.TryGetProperty("file", out var f) || f.GetString() is not { Length: > 0 } file) continue;

                uint offset = overlay.TryGetProperty("offset", out var o) ? o.GetUInt32() : 0;
                uint skip = overlay.TryGetProperty("skip", out var k) ? k.GetUInt32() : 0;
                uint size = overlay.TryGetProperty("size", out var z) ? z.GetUInt32() : 0;
                uint end = offset + skip + size;

                if (floors.TryGetValue(file, out var had)) floors[file] = Math.Max(had, end);
                else
                {
                    order.Add(file);
                    floors[file] = end;
                }
            }
        }
        catch
        {
            // A payload we cannot parse is a broken install, not a bad disc. Nothing
            // is required then, so the disc check passes and the recompile reports
            // the real problem instead of blaming the player's dump.
        }

        return order.Select(f => (f, floors[f])).ToList();
    }
}
