using RecompOne.Recompiler.Analysis;

namespace RecompOne.Recompiler.AutoConfigure;

public sealed record MatchReport(int Named, int Ambiguous, int Total, int ByLayout, List<string> Libraries);

public static class SdkMatcher
{
    public static MatchReport Name(List<MipsFunction> functions, byte[] text, uint textBase, SignatureDb db)
    {
        var words = new uint[text.Length / 4];
        for (var i = 0; i < words.Length; i++) words[i] = BitConverter.ToUInt32(text, i * 4);

        var found = new List<Signature>();
        var anchors = new Dictionary<(string Sdk, string Library, string Object), (long Seat, int Votes)>();
        var votes = new Dictionary<(string Sdk, string Library, string Object), Dictionary<long, int>>();
        var pending = new List<(MipsFunction Fn, int Start, int Count)>();

        int named = 0, ambiguous = 0, byLayout = 0;
        var libraries = new HashSet<string>();
        var options = new Dictionary<MipsFunction, List<string>>();

        foreach (var fn in functions)
        {
            if (fn.Start < textBase || fn.End <= fn.Start) continue;

            var start = (int)(fn.Start - textBase) / 4;
            var count = (int)(fn.End - fn.Start) / 4;
            if (start < 0 || count <= 0 || start + count > words.Length) continue;

            db.Collect(words.AsSpan(start, count), found);
            if (found.Count == 0) continue;

            var name = found[0].Name;
            var single = true;
            foreach (var hit in found)
                if (hit.Name != name)
                {
                    single = false;
                    break;
                }

            options[fn] = found.Select(h => h.Name).Distinct().ToList();

            if (!single)
            {
                ambiguous++;
                pending.Add((fn, start, count));
                continue;
            }

            fn.Name = name;
            named++;
            libraries.Add(found[0].Library);
            Vote(votes, found, fn.Start);
        }

        var score = new Dictionary<string, int>();
        var pick = new Dictionary<string, Signature>();
        //not so good but it helps with disambiguity by using commong anchor functions
        while (pending.Count > 0)
        {
            Settle(votes, anchors);

            var solved = 0;

            for (var i = pending.Count - 1; i >= 0; i--)
            {
                var (fn, start, count) = pending[i];
                db.Collect(words.AsSpan(start, count), found);

                score.Clear();
                pick.Clear();

                foreach (var hit in found)
                foreach (var sdk in hit.Sdk)
                {
                    if (!anchors.TryGetValue((sdk, hit.Library, hit.Object), out var anchor)) continue;
                    if (anchor.Seat != fn.Start - hit.Offset) continue;

                    score.TryGetValue(hit.Name, out var held);
                    if (anchor.Votes <= held) continue;

                    score[hit.Name] = anchor.Votes;
                    pick[hit.Name] = hit;
                }

                if (score.Count == 0) continue;

                string? best = null;
                int top = 0, runnerUp = 0;

                foreach (var (candidate, seen) in score)
                    if (seen > top)
                    {
                        runnerUp = top;
                        top = seen;
                        best = candidate;
                    }
                    else if (seen > runnerUp)
                    {
                        runnerUp = seen;
                    }

                if (best == null || top <= runnerUp) continue;

                fn.Name = best;
                named++;
                byLayout++;
                ambiguous--;
                libraries.Add(pick[best].Library);
                Vote(votes, found, fn.Start, best);

                pending.RemoveAt(i);
                solved++;
            }

            if (solved == 0) break;
        }

        named += NameByCallee(functions, options, ref ambiguous);

        return new MatchReport(named, ambiguous, functions.Count, byLayout, libraries.OrderBy(l => l).ToList());
    }

    private static int NameByCallee(List<MipsFunction> functions, Dictionary<MipsFunction, List<string>> options,
        ref int ambiguous)
    {
        var byStart = new Dictionary<uint, MipsFunction>();
        foreach (var fn in functions) byStart.TryAdd(fn.Start, fn);

        var gained = 0;

        foreach (var (fn, names) in options)
        {
            if (names.Count < 2) continue;
            if (!SoleCall(fn, byStart, out var callee)) continue;

            var wanted = Plain(callee.Name);
            string? match = null;

            foreach (var name in names)
            {
                if (Plain(name) != wanted) continue;
                if (match != null)
                {
                    match = null;
                    break;
                }

                match = name;
            }

            if (match == null || fn.Name == match) continue;

            var settled = !fn.Name.StartsWith("func_", StringComparison.Ordinal);
            fn.Name = match;
            if (settled) continue;

            gained++;
            ambiguous--;
        }

        return gained;
    }

    private static bool SoleCall(MipsFunction fn, Dictionary<uint, MipsFunction> byStart, out MipsFunction callee)
    {
        callee = null!;

        foreach (var instr in fn.Instructions)
        {
            if (instr.Word >> 26 != 3) continue;
            if (callee != null!) return false;
            if (!byStart.TryGetValue(instr.JumpTarget, out var target)) return false;
            if (target.Name.StartsWith("func_", StringComparison.Ordinal)) return false;
            callee = target;
        }

        return callee != null!;
    }

    private static string Plain(string name)
    {
        return name.Replace("_", "").ToLowerInvariant();
    }

    private static void Vote(Dictionary<(string, string, string), Dictionary<long, int>> votes, List<Signature> found,
        uint address, string? only = null)
    {
        foreach (var hit in found)
        {
            if (only != null && hit.Name != only) continue;

            foreach (var sdk in hit.Sdk)
            {
                var key = (sdk, hit.Library, hit.Object);
                if (!votes.TryGetValue(key, out var tally)) votes[key] = tally = new Dictionary<long, int>();

                var seat = address - hit.Offset;
                tally.TryGetValue(seat, out var seen);
                tally[seat] = seen + 1;
            }
        }
    }

    private static void Settle(Dictionary<(string, string, string), Dictionary<long, int>> votes,
        Dictionary<(string Sdk, string Library, string Object), (long Seat, int Votes)> anchors)
    {
        anchors.Clear();

        foreach (var (key, tally) in votes)
        {
            var best = 0L;
            var most = 0;

            foreach (var (seat, seen) in tally)
                if (seen > most || (seen == most && seat < best))
                {
                    most = seen;
                    best = seat;
                }

            anchors[key] = (best, most);
        }
    }
}
