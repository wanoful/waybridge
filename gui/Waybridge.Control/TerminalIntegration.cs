using System.Diagnostics;
using Microsoft.UI.Xaml.Controls;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private readonly ToggleSwitch windowsTerminalProfile = Toggle(false);
    private readonly Button openTerminal = IconButton(L("Open remote terminal"), "\uE756");
    private ControlIpcServer? controlIpc;

    internal void StartBackgroundInitialization() =>
        DispatcherQueue.TryEnqueue(async () => await GuardAsync(() => initialization ??= InitializeAsync(refreshCatalog: false)));

    private void InitializeControlIpc(string stateDirectory)
    {
        controlIpc = new ControlIpcServer(
            stateDirectory,
            EnsureConnectedForShellAsync,
            profileId => controller.Connected && controller.ActiveProfileId == profileId);
    }

    private void NotifyTerminalsDisconnected(string reason) => controlIpc?.NotifyDisconnected(reason);

    private Task<string> EnsureConnectedForShellAsync(Guid profileId, CancellationToken cancellationToken)
    {
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        bool queued = DispatcherQueue.TryEnqueue(async () =>
        {
            try
            {
                if (quitting)
                    throw new InvalidOperationException(Tr("Waybridge Control Center is closing."));
                await (initialization ??= InitializeAsync(refreshCatalog: false));
                if (quitting)
                    throw new InvalidOperationException(Tr("Waybridge Control Center is closing."));
                cancellationToken.ThrowIfCancellationRequested();

                var profile = profiles.SingleOrDefault(item => item.Id == profileId)
                    ?? throw new InvalidOperationException(Tr("The workstation for this terminal was removed. Pair it again in Waybridge Control Center."));
                if (profile.ClientId.Length == 0 || profile.CertificateSha256.Length != 64 || string.IsNullOrEmpty(secrets.Read(profile.Id)))
                    throw new InvalidOperationException(Tr("Pair with “{0}” in Waybridge Control Center first.", profile.Name));

                if (controller.Connected && controller.ActiveProfileId == profile.Id && controller.CurrentSession is { } activeSession)
                {
                    completion.TrySetResult(activeSession);
                    return;
                }
                if (busy)
                    throw new InvalidOperationException(Tr("Waybridge is busy. Try opening the terminal again shortly."));

                string? connectedSession = null;
                await RunOperationAsync(async () =>
                {
                    if (controller.IsRunning && controller.ActiveProfileId != profile.Id)
                    {
                        if (!TrayIcon.ConfirmTerminalSwitch(selected.Name, profile.Name))
                            throw new OperationCanceledException(Tr("Canceled switching workstations."));
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
                        throw new InvalidOperationException(Tr("Could not connect to “{0}”.", profile.Name));
                    connectedSession = controller.CurrentSession ?? profile.Session;
                    if (launcherPreferences.SelectedProfile != profile.Id)
                    {
                        launcherPreferences.SelectedProfile = profile.Id;
                        await SaveLauncherPreferencesAsync();
                    }
                }, canCancelConnect: true);

                completion.TrySetResult(connectedSession ?? throw new InvalidOperationException(Tr("Could not connect to “{0}”.", profile.Name)));
            }
            catch (Exception error)
            {
                completion.TrySetException(error);
            }
        });
        if (!queued) completion.TrySetException(new InvalidOperationException(Tr("Waybridge Control Center is unavailable.")));
        return completion.Task;
    }

    private Task OpenRemoteTerminalAsync() => RunOperationAsync(async () =>
    {
        var profile = selected;
        if (!paired) throw new InvalidOperationException(Tr("Pair with “{0}” in Waybridge Control Center first.", profile.Name));
        if (!await ConnectForLaunchAsync(profile)) return;
        string cliPath = Path.Combine(AppContext.BaseDirectory, "waybridge.exe");
        if (!File.Exists(cliPath))
            throw new FileNotFoundException(Tr("waybridge.exe was not found next to Waybridge.Control.exe."), cliPath);
        var info = new ProcessStartInfo(cliPath)
        {
            UseShellExecute = true,
            Arguments = $"shell --connection {profile.Id:D}"
        };
        Process.Start(info)?.Dispose();
    }, canCancelConnect: true);

    private void SyncWindowsTerminalProfiles()
    {
        if (!OperatingSystem.IsWindows()) return;
        if (!string.Equals(Path.GetFullPath(store.DirectoryPath), Path.GetFullPath(LauncherPreferences.DefaultDirectory), StringComparison.OrdinalIgnoreCase))
            return;
        try
        {
            string cliPath = Path.Combine(AppContext.BaseDirectory, "waybridge.exe");
            string iconPath = Path.Combine(AppContext.BaseDirectory, "Assets", "waybridge.png");
            WindowsTerminalFragments.Sync(profiles, cliPath, File.Exists(iconPath) ? iconPath : null);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Inform(Tr("Could not update Windows Terminal profiles: ") + error.Message);
        }
    }
}
