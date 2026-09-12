using System.Text.Json;
using SteamDatabase.ValvePak;
using ValveResourceFormat;
using ValveResourceFormat.IO;
using ValveResourceFormat.ResourceTypes;

if (args.Length is < 3 or > 4)
{
    Console.Error.WriteLine("Usage: CosmeticAssets <pak01_dir.vpk> <cosmetic_catalog.tsv> <Assets/icons directory> [hero]");
    return 1;
}

using var package = new Package();
package.Read(args[0]);
var outputRoot = Path.GetFullPath(args[2]) + Path.DirectorySeparatorChar;
Directory.CreateDirectory(outputRoot);
var images = File.ReadLines(args[1]).Skip(1)
    .Select(line => line.Split('\t'))
    .Where(fields => fields.Length >= 8 && (args.Length < 4 || fields[0] == args[3]))
    .Select(fields => fields[7].Trim().Replace('\\', '/'))
    .Where(image => image.Length > 0)
    .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
var missing = new List<string>();
var failures = new List<string>();
var exported = 0;
var cached = 0;
foreach (var image in images)
{
    try
    {
        if (!image.StartsWith("econ/", StringComparison.Ordinal) || image.Split('/').Any(part => part is ".." or "." or ""))
            throw new InvalidDataException("Invalid inventory image path");
        var output = Path.GetFullPath(Path.Combine(outputRoot, image + ".png"));
        if (!output.StartsWith(outputRoot, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Image path is outside output directory");
        if (File.Exists(output) && new FileInfo(output).Length > 128)
        {
            cached++;
            continue;
        }
        var entry = package.FindEntry("panorama/images/" + image + "_png.vtex_c");
        if (entry == null)
        {
            missing.Add(image);
            continue;
        }
        package.ReadEntry(entry, out byte[] bytes);
        using var stream = new MemoryStream(bytes);
        using var resource = new Resource();
        resource.Read(stream);
        if (resource.DataBlock is not Texture texture)
            throw new InvalidDataException("Inventory icon is not a texture");
        using var bitmap = texture.GenerateBitmap();
        var png = TextureExtract.ToPngImage(bitmap);
        Directory.CreateDirectory(Path.GetDirectoryName(output)!);
        File.WriteAllBytes(output + ".tmp", png);
        File.Move(output + ".tmp", output, true);
        exported++;
        if (exported % 500 == 0)
            Console.WriteLine($"Exported {exported}/{images.Length} icons");
    }
    catch (Exception exception)
    {
        failures.Add($"{image}: {exception.Message}");
    }
}
var report = new { total = images.Length, exported, cached, missing, failures };
File.WriteAllText(Path.Combine(outputRoot, "cosmetic-export-report.json"), JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }));
Console.WriteLine($"Icons: {images.Length} requested, {exported} exported, {cached} cached, {missing.Count} missing, {failures.Count} failed");
foreach (var failure in failures.Take(5))
    Console.Error.WriteLine(failure);
return failures.Count > 0 ? 2 : 0;
