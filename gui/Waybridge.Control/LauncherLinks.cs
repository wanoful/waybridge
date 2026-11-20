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
            Inform(Tr("Waybridge is busy. Try the app link again shortly."));
            return;
        }

        launchLinkInProgress = link;
        try
        {
            await (initialization ??= InitializeAsync(refreshCatalog: false));
            if (quitting) return;
            var profile = profiles.SingleOrDefault(item => item.Id == link.ProfileId)
                ?? throw new InvalidOperationException(Tr("The workstation for this app link was removed. Pair it again and create a new shortcut."));
            if (profile.ClientId.Length == 0 || profile.CertificateSha256.Length != 64 || string.IsNullOrEmpty(secrets.Read(profile.Id)))
                throw new InvalidOperationException(Tr("Pair with “{0}” again to launch apps.", profile.Name));
            if (busy) throw new InvalidOperationException(Tr("Waybridge is busy. Try the app link again shortly."));

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
                    throw new InvalidOperationException(Tr("Could not connect to “{0}”. The app was not launched.", profile.Name));
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
            Error(error is OperationCanceledException ? new IOException(Tr("Could not connect or launch the app. Try the app link again.")) : error);
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
        Inform(Tr("App link copied."));
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
            throw new InvalidOperationException(Tr("Pairing for this workstation has changed. Create a new shortcut."));
        AppShortcuts.Save(link, app.Name, profile.Name, desktop, store.DirectoryPath, icon);
        Inform(desktop ? Tr("Desktop shortcut created.") : Tr("Added to Start → All apps → Waybridge."));
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
