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
    public bool Hdr { get; set; }
    public bool Udp { get; set; }
    public string UdpHost { get; set; } = "";
    public bool ReadOnly { get; set; }
    public bool Ime { get; set; } = true;
    public bool Touch { get; set; }
    public string Clipboard { get; set; } = "both";
    public bool ClipboardImages { get; set; } = true;
    public bool Files { get; set; } = true;
    public bool DragText { get; set; } = true;
    public string BadgePng { get; set; } = "";
    public bool Notifications { get; set; } = true;
    public bool Tray { get; set; } = true;
    public int Volume { get; set; } = 100;
    public string GpuInput { get; set; } = "cpu";
    public void Validate()
    {
        if (BadgePng is null || BadgePng.Length > 32768) throw new ArgumentException("自定义角标过大");
        if (BadgePng.Length > 0)
        {
            try { if (Convert.FromBase64String(BadgePng).Length > 24 * 1024) throw new ArgumentException("自定义角标过大"); }
            catch (FormatException) { throw new ArgumentException("自定义角标编码无效"); }
        }
        if (Codec is not ("auto" or "png" or "h264" or "hevc" or "av1")) throw new ArgumentException("不支持的视频编码");
        if (Fps is < 1 or > 120 || Bitrate is < 1000 or > 200000) throw new ArgumentException("帧率范围 1–120，码率范围 1000–200000 kbps");
        if (Dpi != 0 && Dpi is < 96 or > 384) throw new ArgumentException("缩放范围 100%–400%");
        if (Clipboard is not ("both" or "send" or "receive" or "off")) throw new ArgumentException("无效的剪贴板方向");
        if (Volume is < 0 or > 100 || GpuInput is not ("cpu" or "cuda")) throw new ArgumentException("无效的声音或 GPU 设置");
        if (Hdr && Codec is "h264" or "png") throw new ArgumentException("HDR 需要自动、HEVC 或 AV1 编码");
    }
    [JsonIgnore]
    public string ServerCodec => Codec switch { "h264" => "h264-nvenc", "hevc" => "hevc-nvenc", "av1" => "av1-nvenc", _ => Codec };
}

public sealed record ConnectionProfile
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Name { get; set; } = "Linux 工作站";
    public string Mode { get; set; } = "ssh";
    public string Host { get; set; } = "";
    public int SshPort { get; set; } = 22;
    public string User { get; set; } = "";
    public string IdentityFile { get; set; } = "";
    public string CertificateSha256 { get; set; } = "";
    public string ClientId { get; set; } = "";
    public string ServerBinary { get; set; } = "waybridge";
    public string Session { get; set; } = "waybridge-0";
    public string SessionMode { get; set; } = "auto";
    public int Port { get; set; } = 49090;
    public bool AutoReconnect { get; set; } = true;
    public ClientSettings Settings { get; set; } = new();
    public void Validate()
    {
        if (Id == Guid.Empty || string.IsNullOrWhiteSpace(Name) || Name.Length > 100) throw new ArgumentException("请输入连接名称");
        if (Mode is not ("ssh" or "tunnel" or "direct")) throw new ArgumentException("无效的连接方式");
        if (!Regex.IsMatch(Host, @"^[a-zA-Z0-9\[\]][a-zA-Z0-9_.:\[\]-]{0,252}$")) throw new ArgumentException("请输入主机名、SSH config 别名或 IP 地址");
        if (User.Length > 64 || (User.Length > 0 && !Regex.IsMatch(User, @"^[a-zA-Z0-9_][a-zA-Z0-9_.-]*$"))) throw new ArgumentException("无效的 SSH 用户名");
        if (SshPort is < 1 or > 65535 || Port is < 0 or > 65535 || (Mode != "ssh" && Port == 0)) throw new ArgumentException("无效的端口");
        if (CertificateSha256.Length > 0 && (CertificateSha256.Length != 64 || !CertificateSha256.All(Uri.IsHexDigit))) throw new ArgumentException("无效的 SHA-256 证书指纹");
        if (ClientId.Length > 256 || ClientId.Any(char.IsWhiteSpace)) throw new ArgumentException("无效的客户端标识");
        if (!Regex.IsMatch(Session, @"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$")) throw new ArgumentException("会话名称仅允许字母、数字、下划线与连字符");
        if (SessionMode is not ("auto" or "attach" or "new")) throw new ArgumentException("无效的会话策略");
        if (string.IsNullOrWhiteSpace(ServerBinary) || ServerBinary.Length > 4096 || ServerBinary.Contains('\0')) throw new ArgumentException("请输入 Linux 服务端路径");
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
        if (new FileInfo(path).Length > 1024 * 1024) throw new IOException("配置文件过大");
        using var stream = File.OpenRead(path);
        var profiles = await JsonSerializer.DeserializeAsync<List<ConnectionProfile>>(stream, Json) ?? [];
        if (profiles.Count > 100 || profiles.Select(p => p.Id).Distinct().Count() != profiles.Count) throw new IOException("配置包含重复或过多连接");
        return profiles;
    }
    public async Task SaveAsync(IEnumerable<ConnectionProfile> profiles)
    {
        var items = profiles.ToList();
        if (items.Count > 100) throw new ArgumentException("最多保存 100 个连接");
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

public sealed record RemoteSession(string Name, int Pid, string Started, int Port, bool Created);
public sealed record RemoteApplication(string Id, string Name, string Icon);
public sealed record TrustRequest(string Host, int Port, IReadOnlyList<string> Fingerprints);
