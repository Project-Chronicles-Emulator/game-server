using CUE4Parse.FileProvider;
using CUE4Parse.UE4.Versions;
using System.Text.Json;
using Serilog;
using CUE4Parse.UE4.Readers;
using CUE4Parse.UE4.IO.Objects;
using System.Buffers.Binary;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.Compression;
using System.Security.Cryptography;

if (args.Length == 1 && args[0] == "--help")
{
    Console.WriteLine("Usage: <Paks> <output> [--extract] [--key FILE] [--oodle DLL]");
    return;
}

const int TocMagicBytes = 16;
const int TocPrefixBytes = 20;
const int CustomTocBytes = 8;
const int NormalizedTocHeaderBytes = 144;

Log.Logger = new LoggerConfiguration().MinimumLevel.Debug().WriteTo.Console().CreateLogger();

if (args.Length < 2)
{
    throw new ArgumentException(
        "Usage: <Paks> <output> [--extract] [--extract-prefix <path>] " +
        "[--key <key-file>] [--oodle <decoder-dll>]");
}

string Option(string name, string fallback)
{
    var index = Array.IndexOf(args, name);
    if (index < 0)
    {
        return fallback;
    }

    if (index + 1 >= args.Length)
    {
        throw new ArgumentException("Missing value for " + name);
    }

    return args[index + 1];
}

var archiveRoot = Path.GetFullPath(args[0]).TrimEnd(Path.DirectorySeparatorChar);
var outputRoot = Path.GetFullPath(args[1]).TrimEnd(Path.DirectorySeparatorChar);
if (outputRoot.Equals(archiveRoot, StringComparison.OrdinalIgnoreCase) ||
    outputRoot.StartsWith(
        archiveRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
{
    throw new ArgumentException("Output must be outside the source archive directory.");
}

if (Directory.Exists(outputRoot) && Directory.EnumerateFileSystemEntries(outputRoot).Any())
{
    throw new ArgumentException("Choose a new, empty extraction directory to preserve existing snapshots.");
}

Directory.CreateDirectory(outputRoot);
using var provider = new DefaultFileProvider(args[0], SearchOption.TopDirectoryOnly,
    new VersionContainer(EGame.GAME_UE5_7), StringComparer.OrdinalIgnoreCase);
foreach (var tocPath in Directory.GetFiles(args[0], "*.utoc"))
{
    var original = File.ReadAllBytes(tocPath);
    if (original.Length < NormalizedTocHeaderBytes + CustomTocBytes ||
        !original.AsSpan(0, TocMagicBytes).SequenceEqual("X5=IoStore=Toc=!"u8))
    {
        throw new InvalidDataException("Unexpected Chronicles TOC signature");
    }

    // This build inserts metadata after the TOC prefix; normalize only that header.
    var normalized = new byte[original.Length - CustomTocBytes];
    original.AsSpan(0, TocPrefixBytes).CopyTo(normalized);
    original.AsSpan(TocPrefixBytes + CustomTocBytes).CopyTo(normalized.AsSpan(TocPrefixBytes));
    FIoStoreTocHeader.TOC_MAGIC.CopyTo(normalized, 0);
    BinaryPrimitives.WriteUInt32LittleEndian(normalized.AsSpan(TocPrefixBytes), NormalizedTocHeaderBytes);
    provider.RegisterVfs(new FByteArchive(tocPath, normalized, provider.Versions), null,
        path => new FStreamArchive(path, File.OpenRead(path), provider.Versions));
}
foreach (var pakPath in Directory.GetFiles(args[0], "*.pak"))
{
    provider.RegisterVfs(new FStreamArchive(pakPath, new FooterStream(pakPath), provider.Versions), null);
}

provider.Mount();
var candidates = Option("--key", Path.Combine(args[1], "archive-key.local.txt"));
if (File.Exists(candidates))
{
    foreach (var candidate in File.ReadAllLines(candidates))
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
}

foreach (var vfs in provider.MountedVfs.Concat(provider.UnloadedVfs))
{
    Console.WriteLine($"{vfs.Name}: encrypted={vfs.IsEncrypted}, files={vfs.Files.Count}, keyGuid={vfs.EncryptionKeyGuid}");
}

Console.WriteLine($"Mounted={provider.MountedVfs.Count}, unloaded={provider.UnloadedVfs.Count}, files={provider.Files.Count}");
using var inventory = new StreamWriter(Path.Combine(args[1], "archive-files.jsonl"));
foreach (var file in provider.Files.Values.OrderBy(f => f.Path))
{
    inventory.WriteLine(JsonSerializer.Serialize(new
    {
        path = file.Path,
        size = file.Size,
        encrypted = file.IsEncrypted
    }));
}

inventory.Flush();
if (provider.Files.Count == 0)
{
    throw new InvalidDataException("No archives mounted.");
}

if (args.Contains("--extract"))
{
    var requestedPrefix = Option("--extract-prefix", "");
    var decoderPath = Option("--oodle", "");
    if (string.IsNullOrWhiteSpace(decoderPath) || !File.Exists(decoderPath))
    {
        throw new ArgumentException("Extraction requires --oodle <path-to-your-local-decoder>.");
    }
    OodleHelper.Initialize(decoderPath);
    using var manifest = new StreamWriter(Path.Combine(args[1], "extraction-manifest.jsonl"));
    int extractedFiles = 0;
    int failedFiles = 0;
    foreach (var file in provider.Files.Values.OrderBy(f => f.Path))
    {
        var relativePath = file.Path;
        if (!string.IsNullOrEmpty(requestedPrefix))
        {
            if (!relativePath.StartsWith(requestedPrefix, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
        }
        else if (!(relativePath.StartsWith("X5/Content/Data/", StringComparison.OrdinalIgnoreCase)
              || relativePath.StartsWith("X5/Content/Design/CMS/", StringComparison.OrdinalIgnoreCase)
              || relativePath.StartsWith("X5/Content/Design/ResourceTable/JSON/", StringComparison.OrdinalIgnoreCase)
              || relativePath.StartsWith("X5/Content/Localization/", StringComparison.OrdinalIgnoreCase)
              || relativePath.EndsWith(".proto", StringComparison.OrdinalIgnoreCase)))
        {
            continue;
        }

        var destinationPath = Path.GetFullPath(Path.Combine(args[1], "raw", relativePath));
        var outputPrefix = Path.GetFullPath(Path.Combine(args[1], "raw")) + Path.DirectorySeparatorChar;
        if (!destinationPath.StartsWith(outputPrefix, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(relativePath);
        }

        try
        {
            var bytes = file.Read();
            if (bytes.LongLength != file.Size)
            {
                throw new InvalidDataException("Size mismatch: " + relativePath);
            }

            Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
            File.WriteAllBytes(destinationPath, bytes);
            manifest.WriteLine(JsonSerializer.Serialize(new
            {
                path = relativePath,
                size = bytes.LongLength,
                sha256 = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
                status = "ok"
            }));
            if (++extractedFiles % 500 == 0)
            {
                Console.WriteLine($"Extracted {extractedFiles} files");
            }
        }
        catch (Exception ex)
        {
            failedFiles++;
            manifest.WriteLine(JsonSerializer.Serialize(new
            {
                path = relativePath,
                status = "failed",
                error = ex.Message
            }));
            Console.WriteLine($"FAILED {relativePath}: {ex.Message}");
        }
    }
    Console.WriteLine($"Extracted={extractedFiles}, failed={failedFiles}");
    if (failedFiles > 0)
    {
        Environment.ExitCode = 2;
    }
}
