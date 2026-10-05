using RecompOne.Recompiler.Analysis;

namespace RecompOne.Recompiler.CodeGen;

public static class SdkPatches
{
    private static readonly (string Key, string Class, string[] Names)[] Libraries =
    {
        ("libcd", "RecompOne.Runtime.Sdk.LibCd", new[]
        {
            "CdInit", "CdReset", "CdControl", "CdControlF", "CdControlB",
            "CdSync", "CdReady", "CdRead", "CdReadSync", "CdGetSector",
            "CdDataSync", "CdSearchFile", "CdSyncCallback", "CdReadyCallback",
            "CdReadCallback", "CdDataCallback", "CdStatus", "CdMode",
            "CdLastCom", "CdMix", "CdFlush"
        }),
        ("libetc", "RecompOne.Runtime.Sdk.LibEtc", new[]
        {
            "VSync"
        }),
        ("libpress", "RecompOne.Runtime.Sdk.LibPress", new[]
        {
            "DecDCTin", "DecDCTout", "DecDCTinSync",
            "DecDCToutSync", "DecDCToutCallback"
        }),
        ("libgpu", "RecompOne.Runtime.Sdk.LibGpu", new[]
        {
            "DrawOTag", "DrawSync", "PutDrawEnv", "PutDispEnv",
            "LoadImage", "StoreImage", "MoveImage", "ClearImage",
            "SetVideoMode", "GetVideoMode"
        }),
        ("libapi", "RecompOne.Runtime.Sdk.LibApi", new[]
        {
            "DMACallback",
        }),
        ("libcdstream", "RecompOne.Runtime.Sdk.LibCdStream", new[]
        {
            "StSetRing", "StClearRing", "StUnSetRing", "StSetStream",
            "StSetMask", "StGetNext", "StFreeRing", "StGetBackloc"
        }),
        ("libmcrd", "RecompOne.Runtime.Sdk.LibMcrd", new[]
        {
            "MemCardInit", "MemCardEnd", "MemCardStart", "MemCardStop",
            "MemCardExist", "MemCardAccept", "MemCardOpen", "MemCardClose",
            "MemCardReadData", "MemCardWriteData", "MemCardReadFile",
            "MemCardWriteFile", "MemCardCreateFile", "MemCardDeleteFile",
            "MemCardFormat", "MemCardUnformat", "MemCardGetDirentry",
            "MemCardSync", "MemCardCallback"
        }),
        ("libds", "RecompOne.Runtime.Sdk.LibDs", new[]
        {
            "DsInit", "DsReset", "DsClose", "DsSetDebug",
            "DsCommand", "DsPacket", "DsSync", "DsFlush", "DsQueueLen",
            "DsControl", "DsControlB", "DsControlF",
            "DsSystemStatus", "DsStatus", "DsShellOpen", "DsLastCom", "DsLastPos",
            "DsIntToPos", "DsPosToInt", "DsMix",
            "DsSyncCallback", "DsReadyCallback", "DsReadCallback", "DsDataCallback",
            "DsStartReadySystem", "DsEndReadySystem", "DsReadySystemMode",
            "DsRead", "DsRead2", "DsReadSync", "DsReadBreak", "DsReadMode", "DsReadFile",
            "DsReady", "DsGetSector", "DsGetSector2", "DsDataSync",
            "DsSearchFile", "DsGetToc", "DsGetDiskType", "DsPlay",
            "DsComstr", "DsIntstr"
        }),
        ("libpad", "RecompOne.Runtime.Sdk.LibPad", new[]
        {
            "PadInitDirect", "PadStartCom", "PadStopCom", "PadEnableCom",
            "PadChkVsync", "PadChkMtap", "PadGetState", "PadInfoMode",
            "PadInfoAct", "PadInfoComb", "PadSetMainMode", "PadSetActAlign",
            "PadSetAct"
        })
    };

    private static readonly (string Key, string Target, string[] Names)[] Aliases =
    {
        ("libcd", "RecompOne.Runtime.Sdk.LibCd.CdSync", new[]
        {
            "CD_sync"
        }),
        ("libcd", "RecompOne.Runtime.Sdk.LibCd.CdReady", new[]
        {
            "CD_ready"
        }),
        ("libcd", "RecompOne.Runtime.Sdk.LibCd.CdDataSync", new[]
        {
            "CD_datasync"
        }),
        ("libcd", "RecompOne.Runtime.Sdk.LibCd.CdGetSector", new[]
        {
            "CD_getsector", "CdGetSector2", "CD_getsector2"
        }),
        ("libapi", "RecompOne.Runtime.Sdk.LibApi.PatchCard", new[]
        {
            "_patch_card"
        }),
        ("libapi", "RecompOne.Runtime.Sdk.LibApi.PatchCard2", new[]
        {
            "_patch_card2"
        }),
        ("libapi", "RecompOne.Runtime.Sdk.LibApi.PatchedBiosCall", new[]
        {
            "_patch_pad_call", "_patch_pad_call2", "_patch_card_call"
        })
    };

    public static void Apply(List<MipsFunction> funcs, IEnumerable<string>? disabled = null)
    {
        var off = new HashSet<string>(disabled ?? [], StringComparer.OrdinalIgnoreCase);
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        var libds = !off.Contains("libds") && funcs.Any(f => f.Name is "DsInit" or "DsCommand" or "DsPacket");
        foreach (var (key, cls, names) in Libraries)
        {
            if (off.Contains(key)) continue;
            foreach (var name in names)
                map[name] = $"{cls}.{name}";
        }

        foreach (var (key, target, names) in Aliases)
        {
            if (off.Contains(key)) continue;
            foreach (var name in names)
                map[name] = target;
        }

        //if (libds) Console.WriteLine("[recompiler] libds detected");

        if (off.Count > 0)
            Console.WriteLine($"[Recompiler] hle impl disabled for: {string.Join(", ", off)}");

        var applied = 0;
        foreach (var func in funcs)
        {
            if (func.IsPatch || func.IsStub) continue;
            if (map.TryGetValue(func.Name, out var target))
            {
                func.IsPatch = true;
                func.PatchTarget = target;
                applied++;
            }
        }

        Console.WriteLine($"[Recompiler] it was applied {applied} reimplementations");
    }
}