using System.Text.Json;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed class LauncherPreferences
{
    public Guid? SelectedProfile { get; set; }
    public bool CloseToTray { get; set; } = true;
    public string Theme { get; set; } = "system";
    public bool CompactApps { get; set; }
    public Dictionary<Guid, List<RemoteApplication>> Recent { get; init; } = [];
    public Dictionary<Guid, List<RemoteApplication>> Pinned { get; init; } = [];
    public Dictionary<Guid, Dictionary<string, string>> AppTouchModes { get; init; } = [];
    public Dictionary<Guid, List<LaunchCommand>> Commands { get; init; } = [];

    public static async Task<LauncherPreferences> LoadAsync(string directory)
    {
        string path = Path.Combine(directory, "launcher.json");
        if (!File.Exists(path)) return new();
        if (new FileInfo(path).Length > 512 * 1024) throw new IOException("启动器偏好文件过大");
        await using var stream = File.OpenRead(path);
        var result = await JsonSerializer.DeserializeAsync<LauncherPreferences>(stream, ProfileStore.Json);
        return result is { Recent: not null, Pinned: not null, AppTouchModes: not null, Commands: not null } ? result : new();
    }

    public async Task SaveAsync(string directory)
    {
        Directory.CreateDirectory(directory);
        string path = Path.Combine(directory, "launcher.json");
        string temporary = $"{path}.{Guid.NewGuid():N}.tmp";
        try
        {
            byte[] data = JsonSerializer.SerializeToUtf8Bytes(this, ProfileStore.Json);
            await File.WriteAllBytesAsync(temporary, data);
            File.Move(temporary, path, true);
        }
        finally { File.Delete(temporary); }
    }
}

internal sealed record LaunchCommand(string Text, string? Cwd);
