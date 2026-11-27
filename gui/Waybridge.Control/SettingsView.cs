using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private readonly List<Button> saveButtons = [];
    private readonly List<Button> applyButtons = [];
    private readonly List<Button> reconnectButtons = [];
    private readonly List<TextBlock> profileScopes = [];
    private readonly TextBlock pairedStatus = Muted("");
    private bool paired;

    private UIElement ConnectionPage()
    {
        L("Linux hostname or IP").Bind(host, TextBox.PlaceholderTextProperty);
        session.PlaceholderText = "waybridge-0";
        addProfile.Click += async (_, _) => await GuardAsync(PairDeviceAsync);
        deleteProfile.Click += async (_, _) => await GuardAsync(DeleteProfileAsync);
        revoke.Click += async (_, _) => await GuardAsync(RevokeProfileAsync);
        end.Click += async (_, _) => await GuardAsync(ConfirmEndAsync);
        var advanced = new Expander
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Content = Column(Field(L("Certificate SHA-256 fingerprint"), fingerprint), Field(L("Session name"), session),
                Row(L("On connect"), L("Applies on next connect"), sessionMode))
        };
        L("Certificate and session").Bind(advanced, Expander.HeaderProperty);
        return SettingsPage(L("Connect"), Actions(addProfile, deleteProfile),
            Card(Column(Heading(L("Pair"), 17), pairedStatus, Actions(revoke)), new Thickness(15)),
            Field(L("Device name"), name), Field(L("Address"), host),
            Row(L("Service port"), L("Defaults to 49092. Reconnect to apply."), remotePort),
            advanced, SettingsActions(),
            Card(Column(Heading(L("Remote session"), 17), Muted(L("Disconnecting keeps Linux apps running.")), end), new Thickness(15)));
    }

    private UIElement DisplayPage() => SettingsPage(L("Video"),
        Row(L("Video codec"), L("Request immediately. Actual codec depends on hardware at both ends and on the session."), codec),
        Row(L("Maximum requested frame rate"), L("Request immediately, subject to the Linux session limit."), fps),
        Row(L("Video bitrate per window (Mbps)"), L("Request immediately. Each active window uses its own bandwidth."), bitrate),
        Row(L("Content scaling"), L("Applies immediately. Native Wayland apps follow each window. X11 apps share a scale."), scale),
        Row(L("HDR (experimental)"), L("Requires HDR support at both ends. Start an HDR-capable Linux session to enable it."), hdr),
        Row(L("Server color input"), L("Applies to new Linux sessions."), gpu),
        Card(Column(Heading(L("Current video path"), 17), actual, appliedVideo), new Thickness(15)), SettingsActions(includeApply: true, includeReconnect: true));

    private UIElement AudioPage() => SettingsPage(L("Sound"),
        Row(L("Play remote sound"), L("Turn off to mute immediately. Enabling audio for the first time may require reconnecting."), audio),
        Row(L("Remote volume"), L("Applies immediately to this connection."), volume),
        Row(L("Windows output device"), L("Takes effect after reconnecting."), output),
        Row(L("Forward microphone"), L("Applies to the current connection. Select Waybridge Microphone in Linux apps."), microphone),
        microphoneStatus,
        SettingsActions(includeApply: true, includeReconnect: true));

    private UIElement SharingPage() => SettingsPage(L("Sharing and permissions"),
        Row(L("View only"), L("Pause remote input and release all held inputs immediately."), readOnly),
        Row(L("Clipboard sync direction"), L("Applies instantly."), clipboard),
        Row(L("Sync clipboard images"), L("Applies immediately. Text sync uses the direction above."), images),
        Row(L("Drag text between devices"), L("Applies immediately and is independent of clipboard sync."), dragText),
        Row(L("Share files and folders"), L("Applies immediately. Received files are read-only."), files),
        Row(L("File transfer speed limit (KiB/s)"), L("Use 0 for unlimited or 32–1048576 for a separate limit on uploads and downloads."), fileRate),
        SettingsActions(includeApply: true));

    private UIElement NetworkPage() => SettingsPage(L("Network"),
        Row(L("Reconnect automatically"), L("Resume the same session after a connection drops. Reconnect to apply."), reconnect),
        Row(L("UDP media"), L("Requires a reachable UDP port on Linux. Start a new session to enable it."), udp),
        Field(L("UDP host (optional, applies on reconnect)"), udpHost),
        SettingsActions(includeReconnect: true));

    private UIElement DesktopPage() => SettingsPage(L("Desktop integration"),
        Row(L("Desktop notifications"), L("Applies immediately. Linux app notifications follow Windows notification settings."), notifications),
        Row(L("System media controls"), L("Applies immediately. Shows Linux media playback info and controls in Windows Quick Settings and media keys."), mediaControls),
        Row(L("Remote app tray icons"), L("Applies immediately. Shows remote app tray icons in the Windows notification area."), remoteTray),
        Row(L("Sync profile to Windows Terminal"), L("Adds this workstation to Windows Terminal via a JSON fragment. Save settings to apply."), windowsTerminalProfile),
        BadgeEditor(), SettingsActions(includeApply: true));

    private UIElement SettingsPage(UiText title, params UIElement[] children)
    {
        var scope = Muted("");
        profileScopes.Add(scope);
        return Column([Heading(title), scope, ..children]);
    }

    private UIElement SettingsActions(bool includeApply = false, bool includeReconnect = false, bool automation = false)
    {
        var save = Button(L("Save settings"), !includeApply);
        save.Click += async (_, _) => await GuardAsync(SaveAsync);
        if (automation) Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(save, "InputSettingsSave");
        saveButtons.Add(save);
        var actions = new List<UIElement> { save };
        if (includeApply)
        {
            var apply = Button(L("Apply to current connection"), true);
            apply.Click += async (_, _) => await GuardAsync(ApplyAsync);
            if (automation) Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(apply, "InputSettingsApply");
            applyButtons.Add(apply);
            actions.Add(apply);
        }
        if (includeReconnect)
        {
            var reconnectButton = Button(L("Save and reconnect"));
            reconnectButton.Click += async (_, _) => await GuardAsync(ReconnectAsync);
            reconnectButtons.Add(reconnectButton);
            actions.Add(reconnectButton);
        }
        return Actions(actions.ToArray());
    }

    private async Task ReconnectAsync() => await RunOperationAsync(async () =>
    {
        var profile = ReadEditor();
        await StoreProfileAsync(profile);
        await controller.DisconnectAsync();
        await ConnectProfileAsync(profile);
    }, canCancelConnect: true);

    private async Task PairDeviceAsync()
    {
        if (controller.IsRunning || !initialized) return;
        await RunOperationAsync(async () =>
        {
            var draft = NewProfile();
            var deviceName = new TextBox { Text = draft.Name };
            var address = new TextBox { PlaceholderText = Tr("For example, 192.168.1.10") };
            var port = Number(draft.Port, 1, 65535);
            bool foundAddress = await PromptAsync(Tr("Add workstation"), Tr("Continue"),
                Column(Field(L("Device name"), deviceName), Field(L("Linux address"), address), Field(L("Service port"), port)), () =>
                {
                    draft = draft with { Name = deviceName.Text.Trim(), Host = address.Text.Trim(), Port = checked((int)port.Value) };
                    draft.Validate();
                });
            if (!foundAddress) return;

            var pin = new TextBox { PlaceholderText = Tr("Full SHA-256 certificate fingerprint") };
            var code = new PasswordBox { PlaceholderText = Tr("One-time pairing code") };
            bool hasTrust = await PromptAsync(Tr("Verify the server identity"), Tr("Pair"),
                Column(Heading($"{draft.Host}:{draft.Port}", 17),
                    Text(L("Run waybridge-service pair on Linux or generate a code in the server console. Copy the full certificate fingerprint from a trusted source.")),
                    Field(L("SHA-256 fingerprint (64 hexadecimal characters)"), pin), Field(L("One-time pairing code"), code)), () =>
                {
                    string normalized = pin.Text.Trim().ToLowerInvariant();
                    if (normalized.Length != 64 || normalized.Any(c => !Uri.IsHexDigit(c)))
                        throw new ArgumentException(Tr("Enter the full 64-character SHA-256 fingerprint"));
                    if (code.Password.Trim().Length == 0) throw new ArgumentException(Tr("Enter a one-time pairing code"));
                    draft = draft with { CertificateSha256 = normalized };
                });
            if (!hasTrust) return;
            var credential = await DirectAgent.PairAsync(draft, code.Password.Trim(), Environment.MachineName);
            code.Password = "";
            var profile = draft with { ClientId = credential.ClientId };
            try
            {
                secrets.Write(profile.Id, credential.ClientToken);
                await store.SaveAsync(profiles.Append(profile));
            }
            catch
            {
                try { secrets.Delete(profile.Id); } catch (Exception) { }
                try
                {
                    using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                    await new DirectAgent(profile, credential.ClientToken).CallAsync(new { op = "revoke", revoke_client_id = profile.ClientId }, deadline.Token);
                }
                catch (Exception) { }
                throw;
            }
            loading = true;
            profiles.Add(profile);
            profilePicker.SelectedItem = profile;
            loading = false;
            await SelectProfileAsync(profile);
            ShowPage("apps");
            Inform(Tr("Paired. Credentials are stored in Windows Credential Manager."));
        });
    }

    private async Task<bool> PromptAsync(string title, string action, StackPanel content, Action validate)
    {
        if (activeDialog is not null) throw new InvalidOperationException(Tr("Close the current dialog first"));
        var issue = new InfoBar { Severity = InfoBarSeverity.Error, IsClosable = false };
        content.Children.Add(issue);
        var dialog = new ContentDialog
        {
            XamlRoot = root.XamlRoot, RequestedTheme = root.RequestedTheme,
            Title = title, Content = content, DefaultButton = ContentDialogButton.Primary,
            PrimaryButtonText = action, CloseButtonText = Tr("Cancel")
        };
        dialog.PrimaryButtonClick += (_, args) =>
        {
            try { validate(); }
            catch (Exception error) when (error is ArgumentException or OverflowException)
            { issue.Message = error.Message; issue.IsOpen = true; args.Cancel = true; }
        };
        activeDialog = dialog;
        try { return await dialog.ShowAsync() == ContentDialogResult.Primary; }
        finally { activeDialog = null; }
    }

    private Task DeleteProfileAsync() => RunOperationAsync(async () =>
    {
        if (controller.IsRunning || !profiles.Any()) return;
        var profile = selected;
        if (!await ConfirmAsync(Tr("Remove device?"), Tr("{0} will be removed from this client. You can revoke its authorization on the server.", profile.Name), Tr("Remove"))) return;
        await store.SaveAsync(profiles.Where(item => item.Id != profile.Id));
        loading = true;
        profiles.Remove(profile);
        profilePicker.SelectedIndex = profiles.Count > 0 ? 0 : -1;
        loading = false;
        launcherPreferences.Recent.Remove(profile.Id);
        launcherPreferences.Pinned.Remove(profile.Id);
        launcherPreferences.AppTouchModes.Remove(profile.Id);
        launcherPreferences.AppStreamModes.Remove(profile.Id);
        launcherPreferences.Commands.Remove(profile.Id);
        var remaining = profiles.FirstOrDefault();
        launcherPreferences.SelectedProfile = remaining?.Id;
        Load(remaining ?? NewProfile());
        secrets.Delete(profile.Id);
        SyncWindowsTerminalProfiles();
        await SaveLauncherPreferencesAsync();
        if (remaining is not null) await RefreshApplicationsAsync();
    });

    private Task RevokeProfileAsync() => RunOperationAsync(async () =>
    {
        if (controller.IsRunning || !paired) return;
        var profile = selected;
        if (!await ConfirmAsync(Tr("Unpair this device?"), Tr("This device will lose access to the Linux service."), Tr("Unpair"))) return;
        string credential = secrets.Read(profile.Id) ?? throw new InvalidOperationException(Tr("Pair this device again"));
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await new DirectAgent(profile, credential).CallAsync(new { op = "revoke", revoke_client_id = profile.ClientId }, deadline.Token);
        secrets.Delete(profile.Id);
        await StoreProfileAsync(profile with { ClientId = "" });
        Inform(Tr("Unpaired. Add the device again to pair it."));
    });

    private Task ConfirmEndAsync() => RunOperationAsync(async () =>
    {
        var profileId = selected.Id;
        if (!controller.Connected || controller.ActiveProfileId != profileId) return;
        if (await ConfirmAsync(Tr("End the remote session?"), Tr("This closes Linux apps in the session. Disconnect to leave them running."), Tr("End session")))
            await controller.EndSessionAsync(expectedProfileId: profileId);
    });
}
