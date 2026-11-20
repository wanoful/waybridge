using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace Waybridge.Control.Core;

public sealed record ClientSettings
{
    public string Codec { get; set; } = "auto";
    public int Fps { get; set; } = 60;
    public int Bitrate { get; set; } = 30000;
    public int Dpi { get; set; }
    public bool Audio { get; set; } = true;
    public string AudioDevice { get; set; } = "";
    public bool Microphone { get; set; }
    public bool Hdr { get; set; }
    public bool Udp { get; set; }
    public string UdpHost { get; set; } = "";
    public bool ReadOnly { get; set; }
    public bool Ime { get; set; } = true;
    public bool Touch { get; set; }
    public bool Pen { get; set; }
    public double PenPressureGamma { get; set; } = 1;
    public bool Gamepads { get; set; }
    public bool GamepadRumble { get; set; } = true;
    public int GamepadDeadzone { get; set; } = 15;
    public Dictionary<string, string> TouchModes { get; set; } = new(StringComparer.Ordinal);
    public Dictionary<string, string> AppStreamModes { get; set; } = new(StringComparer.Ordinal);
    public string Clipboard { get; set; } = "both";
    public bool ClipboardImages { get; set; } = true;
    public bool Files { get; set; } = true;
    public int FileRateKib { get; set; }
    public bool DragText { get; set; } = true;
    public string BadgePng { get; set; } = "";
    public bool Notifications { get; set; } = true;
    public bool Tray { get; set; } = true;
    public int Volume { get; set; } = 100;
    public string GpuInput { get; set; } = "cpu";
    public void Validate()
    {
        if (FileRateKib != 0 && FileRateKib is < 32 or > 1048576)
            throw new ArgumentException(Tr("File transfer speed must be 0 or 32–1048576 KiB/s"));
        if (!double.IsFinite(PenPressureGamma) || PenPressureGamma is < .25 or > 4)
            throw new ArgumentException(Tr("Pen pressure curve must be between 0.25 and 4"));
        if (GamepadDeadzone is < 0 or > 50) throw new ArgumentException(Tr("Controller stick dead zone must be between 0% and 50%"));
        if (TouchModes is null || TouchModes.Count > 100 || TouchModes.Any(p =>
            string.IsNullOrWhiteSpace(p.Key) || p.Key.Length > 256 || p.Key.Any(char.IsControl) || p.Value is not ("native" or "mouse")))
            throw new ArgumentException(Tr("Invalid per-app touch modes. Configure up to 100 apps."));
        if (AppStreamModes is null || AppStreamModes.Count > 256 || AppStreamModes.Any(p =>
            string.IsNullOrEmpty(p.Key) || System.Text.Encoding.UTF8.GetByteCount(p.Key) > 1024
            || p.Key.Any(char.IsControl) || p.Value is not ("clear" or "low_latency")))
            throw new ArgumentException(Tr("Invalid per-app streaming preset"));
        if (BadgePng is null || BadgePng.Length > 32768) throw new ArgumentException(Tr("The custom badge is too large"));
        if (BadgePng.Length > 0)
        {
            try { if (Convert.FromBase64String(BadgePng).Length > 24 * 1024) throw new ArgumentException(Tr("The custom badge is too large")); }
            catch (FormatException) { throw new ArgumentException(Tr("The custom badge encoding is invalid")); }
        }
        if (Codec is not ("auto" or "png" or "h264" or "hevc" or "av1")) throw new ArgumentException(Tr("Unsupported video codec"));
        if (Fps is < 1 or > 120 || Bitrate is < 1000 or > 200000) throw new ArgumentException(Tr("Frame rate must be 1–120 and bitrate must be 1000–200000 kbps"));
        if (Dpi != 0 && Dpi is < 96 or > 384) throw new ArgumentException(Tr("Scaling must be between 100% and 400%"));
        if (Clipboard is not ("both" or "send" or "receive" or "off")) throw new ArgumentException(Tr("Invalid clipboard direction"));
        if (Volume is < 0 or > 100 || GpuInput is not ("cpu" or "cuda")) throw new ArgumentException(Tr("Invalid audio or GPU settings"));
        if (Hdr && Codec is "h264" or "png") throw new ArgumentException(Tr("HDR requires Auto, HEVC, or AV1"));
    }
    [JsonIgnore]
    public string ServerCodec => Codec switch { "h264" => "h264-nvenc", "hevc" => "hevc-nvenc", "av1" => "av1-nvenc", _ => Codec };
}

public sealed record ConnectionProfile
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Name { get; set; } = Tr("Linux workstation");
    public string Host { get; set; } = "";
    public int Port { get; set; } = 49092;
    public string CertificateSha256 { get; set; } = "";
    public string ClientId { get; set; } = "";
    public string Session { get; set; } = "waybridge-0";
    public string SessionMode { get; set; } = "auto";
    public bool AutoReconnect { get; set; } = true;
    public ClientSettings Settings { get; set; } = new();
    internal string Address => Host.StartsWith('[') && Host.EndsWith(']') ? Host[1..^1] : Host;
    public void Validate()
    {
        if (Id == Guid.Empty || string.IsNullOrWhiteSpace(Name) || Name.Length > 100) throw new ArgumentException(Tr("Enter a connection name"));
        if (Host is null || (!Regex.IsMatch(Host, @"\A[a-zA-Z0-9][a-zA-Z0-9_.-]{0,252}\z") &&
            !(Address.AsSpan().IndexOfAny('[', ']') < 0 && IPAddress.TryParse(Address, out var ip) &&
                ip.AddressFamily == AddressFamily.InterNetworkV6)))
            throw new ArgumentException(Tr("Enter a hostname or IP address"));
        if (Port is < 1 or > 65535) throw new ArgumentException(Tr("Invalid port"));
        if (CertificateSha256.Length > 0 && (CertificateSha256.Length != 64 || !CertificateSha256.All(Uri.IsHexDigit))) throw new ArgumentException(Tr("Invalid SHA-256 certificate fingerprint"));
        if (ClientId.Length > 256 || ClientId.Any(char.IsWhiteSpace)) throw new ArgumentException(Tr("Invalid client ID"));
        if (!Regex.IsMatch(Session, @"\A[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}\z")) throw new ArgumentException(Tr("Session names may contain only letters, digits, underscores, and hyphens"));
        if (SessionMode is not ("auto" or "attach" or "new")) throw new ArgumentException(Tr("Invalid session policy"));
        Settings.Validate();
    }
}

public sealed class ProfileStore(string directory)
{
    public static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true
    };
    public string DirectoryPath { get; } = directory;
    public async Task<List<ConnectionProfile>> LoadAsync()
    {
        var path = Path.Combine(DirectoryPath, "profiles.json");
        if (!File.Exists(path)) return [];
        if (new FileInfo(path).Length > 1024 * 1024) throw new IOException(Tr("The profile file is too large"));
        using var stream = File.OpenRead(path);
        var profiles = await JsonSerializer.DeserializeAsync<List<ConnectionProfile>>(stream, Json) ?? [];
        if (profiles.Count > 100 || profiles.Select(p => p.Id).Distinct().Count() != profiles.Count) throw new IOException(Tr("The profile file contains duplicate or too many connections"));
        return profiles;
    }
    public async Task SaveAsync(IEnumerable<ConnectionProfile> profiles)
    {
        var items = profiles.ToList();
        if (items.Count > 100) throw new ArgumentException(Tr("You can save up to 100 connections"));
        Directory.CreateDirectory(DirectoryPath);
        var path = Path.Combine(DirectoryPath, "profiles.json");
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await using (var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                await JsonSerializer.SerializeAsync(stream, items, Json);
                await stream.FlushAsync();
                stream.Flush(true);
            }
            File.Move(temp, path, true);
        }
        finally { File.Delete(temp); }
    }
}

public interface ISecretStore
{
    string? Read(Guid profile);
    void Write(Guid profile, string value);
    void Delete(Guid profile);
}

public sealed record RemoteDesktopAction(string Id, string Name, string Comment = "");

public sealed record RemoteApplication(string Id, string Name, string Icon)
{
    public string GenericName { get; init; } = "";
    public string Comment { get; init; } = "";
    public IReadOnlyList<string> Categories { get; init; } = [];
    public IReadOnlyList<string> Keywords { get; init; } = [];
    public IReadOnlyList<RemoteDesktopAction> Actions { get; init; } = [];
    public string StartupWmClass { get; init; } = "";
}
