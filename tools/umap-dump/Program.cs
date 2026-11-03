// Read-only dump of AX5 trigger-volume actors (by FGuid) from the cooked Chronicles tutorial map.
// Usage: UmapDump <installed-X5-Content-Paks> <key-file> <output-dir> <guid-hex32> [--oodle <dll>] [--map <pkg-path>]
// The key file is read but never printed. Nothing is written inside the game installation or the extraction dump.
using System.Buffers.Binary;
using System.Text;
using CUE4Parse.Compression;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider;
using CUE4Parse.MappingsProvider.Usmap;
using CUE4Parse.UE4.IO.Objects;
using CUE4Parse.UE4.Readers;
using CUE4Parse.UE4.Versions;
using Newtonsoft.Json;

if (args.Length < 4)
{
    Console.Error.WriteLine("Usage: UmapDump <paks-dir> <key-file> <output-dir> <guid-hex32> [--oodle <dll>]");
    return 2;
}

string Option(string name, string fallback)
{
    var index = Array.IndexOf(args, name);
    return index >= 0 && index + 1 < args.Length ? args[index + 1] : fallback;
}

var paksDirectory = args[0];
var keyFile = args[1];
var outputDirectory = args[2];
var targetGuids = args[3].Split(',').Select(guid => guid.Replace("-", "").ToLowerInvariant()).ToList();
Directory.CreateDirectory(outputDirectory);

using var provider = new DefaultFileProvider(paksDirectory, SearchOption.TopDirectoryOnly,
    new VersionContainer(EGame.GAME_UE5_7), StringComparer.OrdinalIgnoreCase);
foreach (var tocPath in Directory.GetFiles(paksDirectory, "*.utoc"))
{
    var original = File.ReadAllBytes(tocPath);
    if (!original.AsSpan(0, 16).SequenceEqual("X5=IoStore=Toc=!"u8))
    {
        throw new InvalidDataException("Unexpected Chronicles TOC signature");
    }
    var normalized = new byte[original.Length - 8];
    original.AsSpan(0, 20).CopyTo(normalized);
    original.AsSpan(28).CopyTo(normalized.AsSpan(20));
    FIoStoreTocHeader.TOC_MAGIC.CopyTo(normalized, 0);
    BinaryPrimitives.WriteUInt32LittleEndian(normalized.AsSpan(20), 144);
    provider.RegisterVfs(new FByteArchive(tocPath, normalized, provider.Versions), null,
        path => new FStreamArchive(path, File.OpenRead(path), provider.Versions));
}
foreach (var pakPath in Directory.GetFiles(paksDirectory, "*.pak"))
{
    provider.RegisterVfs(new FStreamArchive(pakPath, new FooterStream(pakPath), provider.Versions), null);
}
provider.Mount();
foreach (var candidate in File.ReadAllLines(keyFile))
{
    var key = new FAesKey(candidate.Trim());
    foreach (var vfs in provider.UnloadedVfs.ToArray())
    {
        if (vfs.IsEncrypted && vfs.TestAesKey(key))
        {
            provider.SubmitKey(vfs.EncryptionKeyGuid, key);
            Console.WriteLine("Validated local archive key.");
        }
    }
}
Console.WriteLine($"Mounted={provider.MountedVfs.Count}, files={provider.Files.Count}");
var usmapPath = Option("--usmap", "");
if (usmapPath.Length > 0)
{
    provider.MappingsContainer = new FileUsmapTypeMappingsProvider(usmapPath);
    Console.WriteLine("Loaded usmap mapping provider.");
}
OodleHelper.Initialize(Option("--oodle", @"D:\PROJETOS\Sharp-Aion2\tmp\skill-locale\repak\oo2core_9_win64.dll"));

var onlyPackage = Option("--only", "");
var findMode = args.Contains("--find");
var dumpIndices = Option("--indices", "").Split(',', StringSplitOptions.RemoveEmptyEntries).Select(int.Parse).ToList();
var mapPackages = provider.Files.Keys
    .Where(path => path.StartsWith("X5/Content/Maps/ArcheWorld_Tutorial/", StringComparison.OrdinalIgnoreCase)
                   && path.EndsWith(".umap", StringComparison.OrdinalIgnoreCase))
    .Where(path => onlyPackage.Length == 0 || path.Equals(onlyPackage, StringComparison.OrdinalIgnoreCase))
    .OrderBy(path => path.Length).ThenBy(path => path).ToList();
Console.WriteLine($"Tutorial map packages: {mapPackages.Count}");

if (findMode)
{
    // Cheap locator: read each decrypted package once and search for the FGuid words (4 little-endian u32) of every target.
    var found = new StringBuilder();
    foreach (var path in mapPackages)
    {
        var bytes = provider.Files[path].Read();
        foreach (var guid in targetGuids)
        {
            var pattern = new byte[16];
            for (var word = 0; word < 4; word++)
            {
                var value = Convert.ToUInt32(guid.Substring(word * 8, 8), 16);
                BinaryPrimitives.WriteUInt32LittleEndian(pattern.AsSpan(word * 4), value);
            }
            var at = bytes.AsSpan().IndexOf(pattern);
            if (at >= 0)
            {
                found.AppendLine($"{guid} {path} offset={at}");
            }
        }
    }
    File.WriteAllText(Path.Combine(outputDirectory, "guid-locations.txt"), found.ToString());
    Console.WriteLine(found.ToString());
    return 0;
}
var report = new StringBuilder();
var matchedActors = new HashSet<string>();
var errors = new Dictionary<string, int>();
var classCounts = new Dictionary<string, int>();
int loaded = 0;
foreach (var path in mapPackages)
{
    try
    {
        var package = provider.LoadPackage(path);
        loaded++;
        foreach (var lazyExport in package.ExportsLazy)
        {
            Lazy<CUE4Parse.UE4.Assets.Exports.UObject> lazy = lazyExport;
            CUE4Parse.UE4.Assets.Exports.UObject export;
            try
            {
                export = lazy.Value;
            }
            catch (Exception exportException)
            {
                var exportKey = "export: " + exportException.GetType().Name + ": " + exportException.Message.Split('\n')[0];
                errors[exportKey] = errors.GetValueOrDefault(exportKey) + 1;
                continue;
            }
            var className = export.ExportType;
            classCounts[className] = classCounts.GetValueOrDefault(className) + 1;
            if (!className.Contains("TriggerVolume", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            var json = JsonConvert.SerializeObject(export, Formatting.Indented);
            var normalizedJson = json.ToLowerInvariant().Replace("-", "");
            if (!targetGuids.Any(guid => normalizedJson.Contains(guid)))
            {
                continue;
            }
            report.AppendLine($"=== {path} :: {className} :: {export.Name}");
            report.AppendLine(json);
            matchedActors.Add(export.Name.ToString());
        }
        // Second pass (the lazy exports are cached): components and sub-objects owned by a matched actor.
        foreach (var lazyChild in package.ExportsLazy)
        {
            try
            {
                var child = lazyChild.Value;
                if (child.Outer != null && matchedActors.Contains(child.Outer.Name.ToString())
                    && !child.ExportType.Contains("TriggerVolume", StringComparison.OrdinalIgnoreCase))
                {
                    report.AppendLine($"--- child of {child.Outer.Name} :: {child.ExportType} :: {child.Name}");
                    report.AppendLine(JsonConvert.SerializeObject(child, Formatting.Indented));
                }
            }
            catch (Exception)
            {
                // already counted in the first pass
            }
        }
        foreach (var index in dumpIndices)
        {
            try
            {
                var indexed = package.ExportsLazy[index].Value;
                report.AppendLine($"--- export[{index}] {indexed.ExportType} :: {indexed.Name}");
                report.AppendLine(JsonConvert.SerializeObject(indexed, Formatting.Indented));
                var template = indexed.Template?.Object?.Value;
                if (template != null)
                {
                    report.AppendLine($"--- template of export[{index}] {template.ExportType} :: {template.Name}");
                    report.AppendLine(JsonConvert.SerializeObject(template, Formatting.Indented));
                }
            }
            catch (Exception indexException)
            {
                report.AppendLine($"--- export[{index}] error: {indexException.GetType().Name}: {indexException.Message.Split('\n')[0]}");
            }
        }
    }
    catch (Exception exception)
    {
        var key = exception.GetType().Name + ": " + exception.Message.Split('\n')[0];
        errors[key] = errors.GetValueOrDefault(key) + 1;
    }
}
File.WriteAllText(Path.Combine(outputDirectory, "actor-dump.txt"), report.ToString());
File.WriteAllText(Path.Combine(outputDirectory, "summary.json"), JsonConvert.SerializeObject(new
{
    mapPackages = mapPackages.Count,
    loaded,
    errors,
    triggerVolumeClasses = classCounts.Where(entry => entry.Key.Contains("Trigger", StringComparison.OrdinalIgnoreCase)),
    classCountTop = classCounts.OrderByDescending(entry => entry.Value).Take(25),
}, Formatting.Indented));
Console.WriteLine($"Loaded {loaded}/{mapPackages.Count}; errors={errors.Count}; dump bytes={report.Length}");
return 0;
