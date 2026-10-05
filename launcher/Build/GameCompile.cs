using System.Reflection;
using System.Reflection.Metadata;   // TryGetRawMetadata, the single-file case
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace Verdite.Launcher.Build;

/// <summary>
/// Compiles the recompiler's output together with the port's own C# into one game
/// assembly.
///
/// TOGETHER is the design, not an accident of convenience. A port reaches into
/// the recompiled code directly: its Program.cs calls Recompiled.Entry.Run, and
/// its patches make static calls to Recompiled.&lt;Game&gt;.func_XXXXXXXX. Splitting
/// the two apart would mean either an interface boundary for every one of those,
/// or routing them through Dispatcher.Call -- which goes through HookManager and
/// so would not be the same call. Compiling them in one pass keeps every one a
/// plain static call and needs no change to a single line of the port. It costs
/// about fifteen seconds, once.
///
/// The options below MUST stay in step with the game's own csproj (the developer
/// path, KingsField2Recomp.csproj and its kind), which compiles these same
/// sources. A difference between the two
/// is a class of bug that only appears in the release: the port's own notes record
/// what losing the frame boundary looks like, and it is not a crash -- it is the
/// whole game running fast from the title onward, silently.
/// </summary>
static class GameCompile
{
    /// <summary>
    /// Emit the game assembly to <paramref name="dllPath"/>. Throws with the
    /// compiler's own diagnostics on failure.
    /// </summary>
    public static void Run(string generatedDir, string dllPath)
    {
        var parse = CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Latest);

        var trees = new List<SyntaxTree> { CSharpSyntaxTree.ParseText(SourceText.From(GlobalUsings, Encoding.UTF8), parse, "GlobalUsings.g.cs") };

        foreach (var file in Directory.EnumerateFiles(generatedDir, "*.cs", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
            trees.Add(Parse(file, parse));

        foreach (var file in Sources.All())
            trees.Add(Parse(file, parse));

        var options = new CSharpCompilationOptions(OutputKind.ConsoleApplication)
            .WithAllowUnsafe(true)                                   // csproj: AllowUnsafeBlocks
            .WithNullableContextOptions(NullableContextOptions.Enable) // csproj: Nullable
            .WithOptimizationLevel(OptimizationLevel.Release)
            .WithPlatform(Platform.AnyCpu)
            .WithConcurrentBuild(true)
            // The recompiled code is machine-written and warns freely -- unreachable
            // branches out of delay slots, unused locals from registers a function
            // never reads. None of it is actionable, and at 170k lines the noise
            // would bury a real diagnostic from the port's own sources.
            .WithGeneralDiagnosticOption(ReportDiagnostic.Suppress)
            .WithSpecificDiagnosticOptions(new Dictionary<string, ReportDiagnostic>
            {
                // Keep the one warning that is really an error here: a game
                // assembly with no entry point loads and then does nothing.
                ["CS5001"] = ReportDiagnostic.Error,
            });

        var compilation = CSharpCompilation.Create(Launcher.Game.GameAssembly, trees, References(), options);

        // Emit beside the destination and rename, so an interrupted or failed build
        // never leaves a half-written game.dll that the next launch would load.
        var tmp = dllPath + ".tmp";
        Directory.CreateDirectory(Path.GetDirectoryName(dllPath)!);

        EmitResult result;
        using (var fs = File.Create(tmp))
            result = compilation.Emit(fs, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.Embedded));

        if (!result.Success)
        {
            try { File.Delete(tmp); } catch { }
            throw new InvalidOperationException(Report(result));
        }

        File.Move(tmp, dllPath, overwrite: true);
    }

    static SyntaxTree Parse(string file, CSharpParseOptions parse) =>
        CSharpSyntaxTree.ParseText(SourceText.From(File.ReadAllText(file), Encoding.UTF8), parse, file);

    static string Report(EmitResult result)
    {
        var errors = result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error).ToList();

        var sb = new StringBuilder();
        sb.AppendLine($"The game did not compile ({errors.Count} error{(errors.Count == 1 ? "" : "s")}).");
        foreach (var d in errors.Take(20)) sb.AppendLine($"  {d}");
        if (errors.Count > 20) sb.AppendLine($"  ... and {errors.Count - 20} more.");
        return sb.ToString();
    }

    /// <summary>
    /// ImplicitUsings is an SDK feature, not a compiler one.
    ///
    /// Both csprojs enable it and the SDK answers by generating a GlobalUsings.g.cs
    /// into obj/. Roslyn on its own generates nothing, so without this the port's
    /// 22k lines lose System, System.Linq and the rest, and the build fails in
    /// hundreds of places that look like the port is broken rather than like a
    /// missing file. This is the Microsoft.NET.Sdk set for a non-web project, plus
    /// the csproj's own <c>&lt;Using Include="Verdite.Core" /&gt;</c>.
    /// </summary>
    const string GlobalUsings = """
        global using global::System;
        global using global::System.Collections.Generic;
        global using global::System.IO;
        global using global::System.Linq;
        global using global::System.Net.Http;
        global using global::System.Threading;
        global using global::System.Threading.Tasks;
        global using global::Verdite.Core;
        """;

    /// <summary>
    /// What the game assembly compiles against.
    ///
    /// The host's trusted-platform list, which is every assembly it will resolve at
    /// run time -- the whole framework plus RecompOne.Runtime and its dependencies.
    /// Using it rather than AppDomain.GetAssemblies is the difference between a
    /// reference set that is complete and one that is whatever the launcher has
    /// happened to touch: the CLR loads lazily, so System.Net.Sockets is absent
    /// from a walk of loaded assemblies purely because the launcher does not open a
    /// socket -- and patches/AgentServer.cs does. That failure is not subtle when it
    /// happens, but it is invisible until the one patch that needs the missing
    /// assembly is compiled, which is exactly the wrong time to find it.
    ///
    /// ModCompiler walks loaded assemblies instead, and is right to: a mod compiles
    /// against what the game has, and by then the game has loaded it.
    ///
    /// The fallback is the single-file case, where an assembly has no path on disk
    /// and TryGetRawMetadata is the only way to reach its metadata.
    /// </summary>
    static unsafe List<MetadataReference> References()
    {
        var refs = new List<MetadataReference>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        if (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") is string tpa && tpa.Length > 0)
        {
            foreach (var path in tpa.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                var name = Path.GetFileNameWithoutExtension(path);
                if (Skip(name)) continue;
                if (!seen.Add(name)) continue;
                if (!File.Exists(path)) continue;

                try { refs.Add(MetadataReference.CreateFromFile(path)); }
                catch { /* A native library that happens to be listed is not metadata. */ }
            }
        }

        foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (a.IsDynamic) continue;

            var name = a.GetName().Name ?? "";
            if (Skip(name)) continue;
            if (!seen.Add(name)) continue;

            if (!string.IsNullOrEmpty(a.Location))
            {
                refs.Add(MetadataReference.CreateFromFile(a.Location));
                continue;
            }

            if (a.TryGetRawMetadata(out byte* blob, out int length))
                refs.Add(AssemblyMetadata.Create(ModuleMetadata.CreateFromMetadata((IntPtr)blob, length)).GetReference());
        }

        if (refs.Count == 0)
            throw new InvalidOperationException("No reference assemblies were found; this install is incomplete.");

        return refs;
    }

    /// <summary>
    /// The launcher's own assemblies, which the game must not compile against.
    ///
    /// It references the recompiler so it can drive it in process; nothing in the
    /// port names it, and leaving it in the set would let a source file start
    /// depending on it without that being a decision anybody made. Roslyn is NOT
    /// excluded: RecompOne.Runtime references it for ModCompiler, so dropping it
    /// would risk failing on whatever the runtime exposes.
    /// </summary>
    static bool Skip(string name) => name == "recompone" || name == Own;

    /// <summary>This launcher's assembly, Verdite2 or Verdite3: core's launcher/ compiles into it.</summary>
    static readonly string Own = typeof(GameCompile).Assembly.GetName().Name ?? "";
}
