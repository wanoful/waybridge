using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Waybridge.Control.Core;

public static class WindowsTerminalFragments
{
    // Official Windows Terminal third-party fragment namespace UUID.
    private static readonly Guid TerminalNamespace = new("f65ddb7e-706b-4499-8a50-40313caf510a");
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false);

    public static string DefaultDirectory() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Microsoft", "Windows Terminal", "Fragments", "Waybridge");

    public static Guid GenerateProfileGuid(Guid profileId)
    {
        byte[] ns = ToBigEndianBytes(TerminalNamespace);
        byte[] name = Encoding.UTF8.GetBytes("waybridge:profile:" + profileId.ToString("D").ToLowerInvariant());
        byte[] combined = new byte[ns.Length + name.Length];
        Buffer.BlockCopy(ns, 0, combined, 0, ns.Length);
        Buffer.BlockCopy(name, 0, combined, ns.Length, name.Length);
        byte[] hash = SHA1.HashData(combined);
        byte[] result = new byte[16];
        Buffer.BlockCopy(hash, 0, result, 0, 16);
        result[6] = (byte)((result[6] & 0x0F) | 0x50); // Version 5
        result[8] = (byte)((result[8] & 0x3F) | 0x80); // RFC 4122 variant
        return FromBigEndianBytes(result);
    }

    public static string? BuildJson(IEnumerable<ConnectionProfile> profiles, string cliExecutablePath, string? iconPath)
    {
        var entries = new List<Dictionary<string, object>>();
        foreach (var profile in profiles)
        {
            if (!profile.Settings.WindowsTerminalProfile) continue;
            if (profile.ClientId.Length == 0 || profile.CertificateSha256.Length != 64) continue;
            var entry = new Dictionary<string, object>
            {
                ["guid"] = "{" + GenerateProfileGuid(profile.Id).ToString("D") + "}",
                ["name"] = $"Waybridge · {profile.Name}",
                ["commandline"] = $"\"{cliExecutablePath}\" shell --connection {profile.Id:D}"
            };
            if (!string.IsNullOrWhiteSpace(iconPath))
                entry["icon"] = iconPath;
            entries.Add(entry);
        }
        if (entries.Count == 0) return null;
        return JsonSerializer.Serialize(new Dictionary<string, object> { ["profiles"] = entries }, JsonOptions) + "\n";
    }

    public static void Sync(
        IEnumerable<ConnectionProfile> profiles,
        string cliExecutablePath,
        string? iconPath,
        string? fragmentsDirectory = null)
    {
        string directory = fragmentsDirectory ?? DefaultDirectory();
        string path = Path.Combine(directory, "profiles.json");
        string? content = BuildJson(profiles, cliExecutablePath, iconPath);
        if (content is null)
        {
            if (File.Exists(path)) File.Delete(path);
            if (Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
                Directory.Delete(directory);
            return;
        }
        Directory.CreateDirectory(directory);
        string temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, content, Utf8NoBom);
            File.Move(temp, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(temp)) File.Delete(temp);
        }
    }

    private static byte[] ToBigEndianBytes(Guid guid)
    {
        byte[] bytes = guid.ToByteArray();
        Array.Reverse(bytes, 0, 4);
        Array.Reverse(bytes, 4, 2);
        Array.Reverse(bytes, 6, 2);
        return bytes;
    }

    private static Guid FromBigEndianBytes(byte[] bytes)
    {
        byte[] copy = (byte[])bytes.Clone();
        Array.Reverse(copy, 0, 4);
        Array.Reverse(copy, 4, 2);
        Array.Reverse(copy, 6, 2);
        return new Guid(copy);
    }
}
