using System.Text.Json;
using System.Runtime.InteropServices;
using System.Threading.Channels;
using Microsoft.Windows.AppNotifications;
using Microsoft.Windows.AppNotifications.Builder;

namespace Waybridge.Control;

/// <summary>
/// Windows owns notification presentation, its notification center and DND.
/// All mutable toast/action state is confined to one bounded worker queue.
/// No notification text, images or activation arguments enter diagnostic logs.
/// </summary>
internal sealed class DesktopNotifications : IAsyncDisposable
{
    private const string Group = "waybridge-remote";
    private readonly Func<object, Task> send;
    private readonly Action<string> status;
    private readonly Channel<object> queue = Channel.CreateBounded<object>(new BoundedChannelOptions(128)
        { SingleReader = true, FullMode = BoundedChannelFullMode.Wait });
    private readonly CancellationTokenSource lifetime = new();
    private readonly Dictionary<uint, Entry> entries = [];
    private readonly AppNotificationManager? manager;
    private readonly string directory;
    private readonly Task worker, polling;
    private bool connected, enabled = true, available, registered;
    private int failed;
    private sealed record Connection(bool Connected);
    private sealed record Sharing(bool Enabled);
    private sealed record Activation(IReadOnlyDictionary<string, string> Arguments);
    private sealed class Entry(JsonElement message, string token, uint platformId, string? icon)
    {
        public JsonElement Message { get; } = message;
        public string Token { get; } = token;
        public uint PlatformId { get; } = platformId;
        public string? Icon { get; } = icon;
        public DateTimeOffset? Missing;
    }
    public DesktopNotifications(Func<object, Task> send, Action<string> status)
    {
        this.send = send; this.status = status;
        directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Waybridge", "notification-icons", Guid.NewGuid().ToString("N"));
        try
        {
            if (!AppNotificationManager.IsSupported()) throw new NotSupportedException();
            manager = AppNotificationManager.Default;
            manager.NotificationInvoked += Invoked;
            manager.Register();
            registered = true;
            available = manager.Setting == AppNotificationSetting.Enabled;
            if (!available) status("Windows 已关闭此应用的通知，桌面通知转发暂停；请在系统通知设置中启用 Waybridge。");
        }
        catch
        {
            available = false;
            status("Windows 原生通知不可用；桌面通知转发已关闭。请检查 Windows 通知设置与 Windows App Runtime。");
        }
        worker = Task.Run(LoopAsync);
        polling = Task.Run(PollAsync);
    }
    private void Enqueue(object value)
    {
        if (Volatile.Read(ref failed) != 0 || lifetime.IsCancellationRequested) return;
        if (!queue.Writer.TryWrite(value) && Interlocked.Exchange(ref failed, 1) == 0)
        {
            queue.Writer.TryComplete();
            status("通知处理队列已满，已停止通知转发；请重新启动控制中心。");
        }
    }
    public void ConnectionChanged(bool value) => Enqueue(new Connection(value));
    public void SharingChanged(bool value) => Enqueue(new Sharing(value));
    public bool Receive(JsonElement message)
    {
        string? type = message.GetProperty("type").GetString();
        if (type is not ("notification" or "notification_close" or "notification_reset")) return false;
        Enqueue(message.Clone()); return true;
    }
    private void Invoked(AppNotificationManager sender, AppNotificationActivatedEventArgs args)
    {
        // Copy only our bounded arguments, not user input or arbitrary toast data.
        var arguments = args.Arguments.Where(p => p.Key is "n" or "a" && p.Value.Length <= 64)
            .ToDictionary(p => p.Key, p => p.Value);
        Enqueue(new Activation(arguments));
    }
    private async Task PollAsync()
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(2));
        try { while (await timer.WaitForNextTickAsync(lifetime.Token)) Enqueue(true); }
        catch (OperationCanceledException) { }
    }
    private async Task LoopAsync()
    {
        try
        {
            // Remove stale notifications belonging to this process identity after
            // a crash. No old toast can act on a newly connected Linux session.
            if (manager is not null) await manager.RemoveByGroupAsync(Group);
            await foreach (var value in queue.Reader.ReadAllAsync(lifetime.Token))
            {
                if (Volatile.Read(ref failed) != 0) break;
                switch (value)
                {
                    case Connection connection:
                        connected = connection.Connected;
                        await ResetAsync();
                        if (connected) await AvailabilityAsync();
                        break;
                    case Sharing sharing:
                        enabled = sharing.Enabled;
                        if (!enabled) await ResetAsync();
                        break;
                    case Activation activation: await ActivateAsync(activation); break;
                    case JsonElement message:
                        if (message.GetProperty("type").GetString() == "notification_reset") await ResetAsync();
                        else if (connected && enabled) await ReceiveAsync(message);
                        break;
                    case bool:
                        if (connected) { await AvailabilityAsync(); await PollDismissalsAsync(); }
                        break;
                }
            }
        }
        catch (OperationCanceledException) { }
        catch
        {
            Interlocked.Exchange(ref failed, 1);
            status("Windows 原生通知服务失败，桌面通知转发已关闭；请检查 Windows App Runtime 和通知权限。");
        }
        finally
        {
            try { await send(new { type = "notification_available", available = false }); } catch { }
            try { await ResetAsync(); } catch { }
        }
    }
    private async Task AvailabilityAsync()
    {
        bool current = manager is not null && manager.Setting == AppNotificationSetting.Enabled;
        if (current != available && !current)
        {
            await ResetAsync();
            status("Windows 已关闭此应用的通知，桌面通知转发暂停。免打扰仍由 Windows 管理。");
        }
        available = current;
        try { await send(new { type = "notification_available", available }); } catch { }
    }
    private async Task ReceiveAsync(JsonElement message)
    {
        uint id = message.GetProperty("id").GetUInt32();
        if (message.GetProperty("type").GetString() == "notification_close")
        {
            if (entries.TryGetValue(id, out var current) && Same(current.Message, message)) await RemoveAsync(id);
            return;
        }
        if (!available || manager is null) { await FeedbackAsync(message, reason: 4); return; }
        if (entries.Count >= 64 && !entries.ContainsKey(id)) { await FeedbackAsync(message, reason: 4); return; }
        entries.TryGetValue(id, out var previous);
        string token = Guid.NewGuid().ToString("N");
        var builder = new AppNotificationBuilder().AddArgument("n", token)
            .AddText(message.GetProperty("app_name").GetString() ?? "Linux 应用")
            .AddText(message.GetProperty("summary").GetString() ?? "")
            .AddText(message.GetProperty("body").GetString() ?? "");
        var actions = message.GetProperty("actions").EnumerateArray().ToArray();
        for (int i = 0; i < actions.Length; i++)
            if (actions[i].GetProperty("key").GetString() != "default")
                builder.AddButton(new AppNotificationButton(actions[i].GetProperty("label").GetString() ?? "操作")
                    .AddArgument("n", token).AddArgument("a", i.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        string? icon = null;
        if (message.TryGetProperty("png", out var png) && png.ValueKind == JsonValueKind.String && png.GetString() is { Length: <= 65536 } encoded)
        {
            var bytes = Convert.FromBase64String(encoded);
            if (bytes.Length is > 8 and <= 49152 && bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137,80,78,71,13,10,26,10 }))
            {
                Directory.CreateDirectory(directory);
                icon = Path.Combine(directory, token + ".png");
                await File.WriteAllBytesAsync(icon, bytes);
                builder.SetAppLogoOverride(new Uri(icon));
            }
        }
        var toast = builder.BuildNotification();
        toast.Group = Group; toast.Tag = id.ToString(System.Globalization.CultureInfo.InvariantCulture);
        toast.ExpiresOnReboot = true;
        int timeout = message.GetProperty("timeout_ms").GetInt32();
        if (timeout > 0) toast.Expiration = DateTimeOffset.Now.AddMilliseconds(timeout);
        try
        {
            manager.Show(toast);
            if (toast.Id == 0) throw new InvalidOperationException();
            entries[id] = new Entry(message.Clone(), token, toast.Id, icon);
            // Same Tag/Group replaces atomically in the Windows platform; do not
            // first remove the visible toast or expose a second notification.
            if (previous?.Icon is not null) File.Delete(previous.Icon);
        }
        catch
        {
            if (icon is not null) File.Delete(icon);
            // The provider has already committed the replacement revision. A
            // failed platform replacement closes that logical notification;
            // keeping the old toast would leave a visible but stale action.
            await RemoveAsync(id);
            await FeedbackAsync(message, reason: 4);
            status("Windows 未接受一条远端通知；通知正文未写入日志。");
        }
    }
    private static bool Same(JsonElement a, JsonElement b) =>
        a.GetProperty("instance").GetString() == b.GetProperty("instance").GetString() &&
        a.GetProperty("binding").GetString() == b.GetProperty("binding").GetString() &&
        a.GetProperty("revision").GetUInt64() == b.GetProperty("revision").GetUInt64();
    private async Task ActivateAsync(Activation activation)
    {
        if (!connected || !enabled || !available || !activation.Arguments.TryGetValue("n", out var token)) return;
        var pair = entries.FirstOrDefault(p => p.Value.Token == token);
        if (pair.Value is null) return;
        var actions = pair.Value.Message.GetProperty("actions").EnumerateArray().ToArray();
        string? action = null;
        if (activation.Arguments.TryGetValue("a", out var index) && int.TryParse(index, out int i) && i >= 0 && i < actions.Length)
            action = actions[i].GetProperty("key").GetString();
        else action = actions.FirstOrDefault(a => a.GetProperty("key").GetString() == "default") is var fallback && fallback.ValueKind != JsonValueKind.Undefined
            ? fallback.GetProperty("key").GetString() : null;
        await FeedbackAsync(pair.Value.Message, action, action is null ? 2u : null);
        await RemoveAsync(pair.Key);
    }
    private async Task FeedbackAsync(JsonElement message, string? action = null, uint? reason = null)
    {
        if (!connected) return;
        var value = new Dictionary<string, object?> { ["type"] = "notification_feedback",
            ["instance"] = message.GetProperty("instance").GetString(), ["binding"] = message.GetProperty("binding").GetString(),
            ["id"] = message.GetProperty("id").GetUInt32(), ["revision"] = message.GetProperty("revision").GetUInt64() };
        if (action is not null) value["action"] = action;
        if (reason is not null) value["reason"] = reason.Value;
        try { await send(value); } catch { }
    }
    private async Task PollDismissalsAsync()
    {
        if (!available || manager is null || entries.Count == 0) return;
        var present = (await manager.GetAllAsync()).Select(n => n.Id).ToHashSet();
        foreach (var pair in entries.ToArray())
        {
            if (present.Contains(pair.Value.PlatformId)) { pair.Value.Missing = null; continue; }
            // AppNotifications has no Dismissed event. Give activation delivery a
            // grace period before translating removal from the notification center.
            pair.Value.Missing ??= DateTimeOffset.UtcNow;
            if (DateTimeOffset.UtcNow - pair.Value.Missing.Value < TimeSpan.FromSeconds(2)) continue;
            await FeedbackAsync(pair.Value.Message, reason: 2); await RemoveAsync(pair.Key);
        }
    }
    private async Task RemoveAsync(uint id)
    {
        if (!entries.Remove(id, out var entry)) return;
        try
        {
            if (manager is not null)
                try { await manager.RemoveByIdAsync(entry.PlatformId); }
                catch (COMException error) when (error.HResult == unchecked((int)0x80070490)) { /* already removed by Windows/user */ }
        }
        finally { if (entry.Icon is not null) File.Delete(entry.Icon); }
    }
    private async Task ResetAsync()
    {
        foreach (uint id in entries.Keys.ToArray()) await RemoveAsync(id);
        if (Directory.Exists(directory)) Directory.Delete(directory, true);
    }
    public async ValueTask DisposeAsync()
    {
        queue.Writer.TryComplete(); lifetime.Cancel();
        try { await Task.WhenAll(worker, polling); } catch (OperationCanceledException) { }
        if (manager is not null)
        {
            manager.NotificationInvoked -= Invoked;
            if (registered)
                try { manager.Unregister(); }
                catch (COMException error) when (error.HResult == unchecked((int)0x80070490)) { /* OS registration already gone */ }
        }
        lifetime.Dispose();
    }
}
