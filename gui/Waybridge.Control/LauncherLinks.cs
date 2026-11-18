using System.Text.Json;
using Waybridge.Control.Core;
using Windows.ApplicationModel.DataTransfer;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private AppLaunchLink? launchLinkInProgress;

    internal void QueueLaunchLink(AppLaunchLink link) =>
        DispatcherQueue.TryEnqueue(async () => await OpenLaunchLinkAsync(link));

    private void ShowControlCenter()
    {
        TrayIcon.ShowWindow(hwnd, 9);
        Activate();
    }

    private async Task OpenLaunchLinkAsync(AppLaunchLink link)
    {
        if (quitting || launchLinkInProgress == link) return;
        if (launchLinkInProgress is not null || busy)
        {
            ShowControlCenter();
            Inform("Waybridge 正在处理另一个操作，请稍后重试应用链接。");
            return;
        }

        launchLinkInProgress = link;
        try
        {
            await (initialization ??= InitializeAsync(refreshCatalog: false));
            if (quitting) return;
            var profile = profiles.SingleOrDefault(item => item.Id == link.ProfileId)
                ?? throw new InvalidOperationException("应用链接对应的工作站已移除。请重新配对并创建快捷方式。");
            if (profile.ClientId.Length == 0 || profile.CertificateSha256.Length != 64 || string.IsNullOrEmpty(secrets.Read(profile.Id)))
                throw new InvalidOperationException($"「{profile.Name}」需要重新配对才能启动应用。");
            if (busy) throw new InvalidOperationException("Waybridge 正在处理另一个操作，请稍后重试应用链接。");

            await RunOperationAsync(async () =>
            {
                if (controller.IsRunning && controller.ActiveProfileId != profile.Id)
                {
                    if (!TrayIcon.ConfirmLinkSwitch(selected.Name, profile.Name)) return;
                    await controller.DisconnectAsync();
                }
                if (selected.Id != profile.Id)
                {
                    Load(profile);
                    loading = true;
                    try { profilePicker.SelectedItem = profile; }
                    finally { loading = false; }
                    applicationSearch.Text = "";
                }
                if (!await ConnectForLaunchAsync(profile))
                    throw new InvalidOperationException($"未连接到「{profile.Name}」，应用尚未启动。");
                var application = applications.FirstOrDefault(app => app.Id == link.AppId);
                await LaunchForProfileAsync(profile, link.AppId, application, shellActivation: true);
                if (launcherPreferences.SelectedProfile != profile.Id)
                {
                    launcherPreferences.SelectedProfile = profile.Id;
                    await SaveLauncherPreferencesAsync();
                }
            }, canCancelConnect: true);
        }
        catch (Exception error)
        {
            if (quitting) return;
            ShowControlCenter();
            Error(error is OperationCanceledException ? new IOException("连接或启动未完成，请重试应用链接。") : error);
        }
        finally { launchLinkInProgress = null; }
    }

    private Task CopyApplicationLinkAsync(RemoteApplication app)
    {
        var link = AppLaunchLink.Create(selected.Id, app.Id);
        AppShortcuts.RegisterProtocol();
        var data = new DataPackage();
        data.SetText(link.ToString());
        Clipboard.SetContent(data);
        Clipboard.Flush();
        Inform("已复制应用链接。");
        return Task.CompletedTask;
    }

    private async Task SaveApplicationShortcutAsync(RemoteApplication app, bool desktop)
    {
        var profile = selected;
        var link = AppLaunchLink.Create(profile.Id, app.Id);
        AppShortcuts.RegisterProtocol();
        byte[]? icon = await ShortcutIconAsync(profile, app.Id);
        if (quitting) return;
        if (!profiles.Any(item => item.Id == profile.Id && item.ClientId == profile.ClientId))
            throw new InvalidOperationException("工作站的配对配置已更改，请重新创建快捷方式。");
        AppShortcuts.Save(link, app.Name, profile.Name, desktop, store.DirectoryPath, icon);
        Inform(desktop ? "已创建桌面快捷方式。" : "已添加到「开始 → 所有应用 → Waybridge」。");
    }

    private async Task<byte[]?> ShortcutIconAsync(ConnectionProfile profile, string id)
    {
        var cached = await ReadCachedIconAsync(profile, id);
        if (cached is not null) return cached;
        var credential = secrets.Read(profile.Id);
        if (credential is null) return null;
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        try
        {
            var reply = await new DirectAgent(profile, credential).CallAsync(new { op = "app_icons", apps = new[] { id } }, timeout.Token);
            if (!reply.GetProperty("icons").TryGetProperty(id, out var entry) || entry.ValueKind != JsonValueKind.Object)
                return null;
            byte[]? icon = DecodeLauncherIcon(entry, out string hash);
            if (icon is not null) await SaveIconAsync(profile, id, hash, icon);
            return icon;
        }
        catch (Exception error) when (error is IOException or OperationCanceledException or System.Net.Sockets.SocketException
            or System.Security.Authentication.AuthenticationException or JsonException or KeyNotFoundException
            or InvalidOperationException or FormatException) { return null; }
    }
}
