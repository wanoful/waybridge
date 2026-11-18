using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Waybridge.Control.Core;
using Windows.Storage.Streams;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private const int MaxLauncherPng = 96 * 1024;
    private readonly Dictionary<string, BitmapImage> launcherIcons = new(StringComparer.Ordinal);
    private readonly Dictionary<string, List<WeakReference<IconSlot>>> iconQueue = new(StringComparer.Ordinal);
    private readonly HashSet<string> missingIcons = new(StringComparer.Ordinal);
    private CancellationTokenSource? iconRequest;
    private bool iconPumpActive;
    private int iconGeneration;
    private sealed record IconSlot(Grid Container, Image Image)
    {
        public void Show(BitmapImage bitmap)
        {
            Image.Source = bitmap;
            if (Image.Parent is not null) return;
            Container.Children.Clear();
            Container.Children.Add(Image);
        }
    }

    private Grid LauncherMark(RemoteApplication? app, double size)
    {
        var fallback = Glyph("\uE71D", size * .55);
        fallback.HorizontalAlignment = HorizontalAlignment.Center;
        fallback.VerticalAlignment = VerticalAlignment.Center;
        var mark = new Grid { Width = size, Height = size, Children = { Card(fallback) } };
        if (app is null) return mark;
        var image = new Image { Width = size, Height = size, Stretch = Stretch.Uniform };
        var slot = new IconSlot(mark, image);
        Guid profileId = selected.Id;
        mark.Loaded += (_, _) =>
        {
            if (profileId == selected.Id) QueueLauncherIcon(app.Id, slot);
        };
        return mark;
    }

    private void InvalidateIcons()
    {
        iconGeneration++;
        iconRequest?.Cancel();
        iconQueue.Clear();
        launcherIcons.Clear();
        missingIcons.Clear();
    }

    private void QueueLauncherIcon(string id, IconSlot target)
    {
        if (launcherIcons.TryGetValue(id, out var image)) { target.Show(image); return; }
        if (missingIcons.Contains(id) || !paired || quitting) return;
        if (!iconQueue.TryGetValue(id, out var targets)) iconQueue[id] = targets = [];
        targets.Add(new(target));
        if (!iconPumpActive)
        {
            iconPumpActive = true;
            if (!DispatcherQueue.TryEnqueue(async () => await PumpLauncherIconsAsync())) iconPumpActive = false;
        }
    }

    private async Task PumpLauncherIconsAsync()
    {
        try
        {
            while (iconQueue.Count > 0 && !quitting)
            {
                int generation = iconGeneration;
                var profile = selected;
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(12));
                iconRequest = deadline;
                var batch = iconQueue.Take(12).ToArray();
                var needed = new List<string>();
                try
                {
                    string? token = secrets.Read(profile.Id);
                    if (token is null) { iconQueue.Clear(); return; }
                    foreach (var (id, _) in batch)
                    {
                        var bytes = await ReadCachedIconAsync(profile, id);
                        if (bytes is null) needed.Add(id);
                        else await DisplayIconAsync(generation, id, bytes);
                    }
                    if (generation != iconGeneration) continue;
                    if (needed.Count > 0)
                    {
                        var server = new DirectAgent(profile, token);
                        var waiting = needed.ToArray();
                        for (int attempt = 0; attempt < 2 && waiting.Length > 0; attempt++)
                        {
                            var payload = await server.CallAsync(new { op = "app_icons", apps = waiting }, deadline.Token);
                            if (generation != iconGeneration) break;
                            var result = payload.GetProperty("icons");
                            foreach (string id in waiting)
                            {
                                if (!result.TryGetProperty(id, out var value)) continue;
                                if (value.ValueKind != JsonValueKind.Object) { missingIcons.Add(id); continue; }
                                string hash = value.GetProperty("sha256").GetString() ?? "";
                                string encoded = value.GetProperty("png").GetString() ?? "";
                                if (!ValidIconHash(hash) || encoded.Length > MaxLauncherPng / 3 * 4) continue;
                                byte[] bytes = Convert.FromBase64String(encoded);
                                if (!ValidLauncherPng(bytes, hash)) continue;
                                await DisplayIconAsync(generation, id, bytes);
                                if (generation != iconGeneration) break;
                                await SaveIconAsync(profile, id, hash, bytes);
                            }
                            var deferred = payload.TryGetProperty("omitted", out var omitted) && omitted.ValueKind == JsonValueKind.Array
                                ? omitted.EnumerateArray().Select(value => value.GetString()).ToHashSet(StringComparer.Ordinal)
                                : [];
                            waiting = waiting.Where(deferred.Contains).ToArray();
                        }
                    }
                }
                catch (Exception)
                {
                    if (generation == iconGeneration)
                        foreach (var id in iconQueue.Keys) missingIcons.Add(id);
                }
                finally
                {
                    if (generation == iconGeneration)
                        foreach (var (id, _) in batch)
                        {
                            iconQueue.Remove(id);
                            if (!launcherIcons.ContainsKey(id)) missingIcons.Add(id);
                        }
                    if (ReferenceEquals(iconRequest, deadline)) iconRequest = null;
                }
            }
        }
        finally { iconPumpActive = false; }
    }

    private async Task DisplayIconAsync(int generation, string id, byte[] bytes)
    {
        if (generation != iconGeneration) return;
        using var memory = new InMemoryRandomAccessStream();
        using var writer = new DataWriter(memory);
        writer.WriteBytes(bytes);
        await writer.StoreAsync();
        writer.DetachStream();
        memory.Seek(0);
        var image = new BitmapImage();
        await image.SetSourceAsync(memory);
        if (generation != iconGeneration) return;
        if (launcherIcons.Count >= 192) launcherIcons.Remove(launcherIcons.Keys.First());
        launcherIcons[id] = image;
        if (iconQueue.TryGetValue(id, out var views))
            foreach (var view in views)
                if (view.TryGetTarget(out var target)) target.Show(image);
    }

    private string IconIndexPath(ConnectionProfile profile, string id)
    {
        var identity = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes($"{profile.CertificateSha256}\n{id}")));
        return Path.Combine(store.DirectoryPath, "cache", "icons", "index", identity);
    }

    private string IconRawPath(string hash) => Path.Combine(store.DirectoryPath, "cache", "icons", "raw", hash + ".png");

    private async Task<byte[]?> ReadCachedIconAsync(ConnectionProfile profile, string id)
    {
        try
        {
            var index = new FileInfo(IconIndexPath(profile, id));
            if (!index.Exists || index.Length != 64 || DateTime.UtcNow - index.LastWriteTimeUtc > TimeSpan.FromDays(1)) return null;
            string hash = await File.ReadAllTextAsync(index.FullName);
            if (!ValidIconHash(hash)) return null;
            var raw = new FileInfo(IconRawPath(hash));
            if (!raw.Exists || raw.Length > MaxLauncherPng) return null;
            byte[] bytes = await File.ReadAllBytesAsync(raw.FullName);
            return ValidLauncherPng(bytes, hash) ? bytes : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return null; }
    }

    private async Task SaveIconAsync(ConnectionProfile profile, string id, string hash, byte[] bytes)
    {
        try
        {
            string raw = IconRawPath(hash), index = IconIndexPath(profile, id);
            Directory.CreateDirectory(Path.GetDirectoryName(raw)!);
            Directory.CreateDirectory(Path.GetDirectoryName(index)!);
            await File.WriteAllBytesAsync(raw, bytes);
            await File.WriteAllTextAsync(index, hash);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
    }

    private static bool ValidIconHash(string hash) => hash.Length == 64 && hash.All(Uri.IsHexDigit);

    private static bool ValidLauncherPng(byte[] bytes, string hash) => bytes.Length is >= 33 and <= MaxLauncherPng
        && bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 })
        && bytes.AsSpan(12, 4).SequenceEqual("IHDR"u8)
        && BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(16, 4)) == 128
        && BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(20, 4)) == 128
        && Convert.ToHexString(SHA256.HashData(bytes)).Equals(hash, StringComparison.OrdinalIgnoreCase);
}
