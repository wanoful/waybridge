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
        host.PlaceholderText = "Linux 主机名或 IP";
        session.PlaceholderText = "waybridge-0";
        addProfile.Click += async (_, _) => await GuardAsync(PairDeviceAsync);
        deleteProfile.Click += async (_, _) => await GuardAsync(DeleteProfileAsync);
        revoke.Click += async (_, _) => await GuardAsync(RevokeProfileAsync);
        end.Click += async (_, _) => await GuardAsync(ConfirmEndAsync);
        var advanced = new Expander
        {
            Header = "证书与会话",
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Content = Column(Field("证书 SHA-256 指纹", fingerprint), Field("会话名称", session),
                Row("打开连接时", "下次连接生效", sessionMode))
        };
        return SettingsPage("连接", Actions(addProfile, deleteProfile),
            Card(Column(Heading("配对", 17), pairedStatus, Actions(revoke)), new Thickness(15)),
            Field("设备名称", name), Field("地址", host),
            Row("服务端口", "默认 49092，重新连接后生效", remotePort),
            advanced, SettingsActions(),
            Card(Column(Heading("远端会话", 17), Muted("断开连接会保留 Linux 应用。"), end), new Thickness(15)));
    }

    private UIElement DisplayPage() => SettingsPage("画面",
        Row("视频编码", "可实时请求，实际编码由两端硬件与会话决定。", codec),
        Row("请求帧率上限", "实时请求，受 Linux 会话上限限制。", fps),
        Row("每窗口视频码率（Mbps）", "实时请求。多个活动窗口分别占用带宽。", bitrate),
        Row("内容缩放", "实时应用。原生 Wayland 跟随各窗口，X11 应用共享缩放。", scale),
        Row("HDR（实验）", "两端支持时可生效。首次启用需要新建支持 HDR 的 Linux 会话。", hdr),
        Row("服务端颜色输入", "新建 Linux 会话时生效。", gpu),
        Card(Column(Heading("当前画面路径", 17), actual, appliedVideo), new Thickness(15)), SettingsActions(includeApply: true, includeReconnect: true));

    private UIElement AudioPage() => SettingsPage("声音",
        Row("播放远端声音", "关闭可即时静音。首次打开音频可能需要重连。", audio),
        Row("远端音量", "实时应用，仅影响本连接。", volume),
        Row("Windows 输出设备", "重连后生效。", output),
        Row("转发麦克风", "应用到当前连接后生效。Linux 应用可选择 Waybridge Microphone。", microphone),
        microphoneStatus,
        SettingsActions(includeApply: true, includeReconnect: true));

    private UIElement SharingPage() => SettingsPage("共享与权限",
        Row("只读查看", "实时暂停远端控制并释放已按下的输入。", readOnly),
        Row("剪贴板同步方向", "实时应用。", clipboard),
        Row("同步剪贴板图片", "实时应用。文字同步按上面的方向设置。", images),
        Row("跨端文字拖放", "实时应用。独立于剪贴板同步。", dragText),
        Row("共享文件和目录", "实时应用。接收的文件以只读方式交付。", files),
        Row("文件传输限速（KiB/s）", "0 为不限速，有效限速为 32–1048576，分别应用于上传和下载。", fileRate),
        SettingsActions(includeApply: true));

    private UIElement NetworkPage() => SettingsPage("网络",
        Row("断线自动接回", "连接恢复后接回原会话。重新连接后生效。", reconnect),
        Row("UDP 媒体", "需要可直达的 Linux UDP 端口。首次启用需要新建会话。", udp),
        Field("UDP 主机（可选，重连后生效）", udpHost),
        SettingsActions(includeReconnect: true));

    private UIElement DesktopPage() => SettingsPage("桌面集成",
        Row("桌面通知", "实时应用。Linux 应用通知遵循 Windows 系统通知设置。", notifications),
        Row("远端应用托盘", "实时应用。在 Windows 通知区显示远端应用的托盘入口。", remoteTray),
        BadgeEditor(), SettingsActions(includeApply: true));

    private UIElement SettingsPage(string title, params UIElement[] children)
    {
        var scope = Muted("");
        profileScopes.Add(scope);
        return Column([Heading(title), scope, ..children]);
    }

    private UIElement SettingsActions(bool includeApply = false, bool includeReconnect = false, bool automation = false)
    {
        var save = Button("保存设置", !includeApply);
        save.Click += async (_, _) => await GuardAsync(SaveAsync);
        if (automation) Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(save, "InputSettingsSave");
        saveButtons.Add(save);
        var actions = new List<UIElement> { save };
        if (includeApply)
        {
            var apply = Button("应用到当前连接", true);
            apply.Click += async (_, _) => await GuardAsync(ApplyAsync);
            if (automation) Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(apply, "InputSettingsApply");
            applyButtons.Add(apply);
            actions.Add(apply);
        }
        if (includeReconnect)
        {
            var reconnectButton = Button("保存并重新连接");
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
            var address = new TextBox { PlaceholderText = "例如 192.168.1.10" };
            var port = Number(draft.Port, 1, 65535);
            bool foundAddress = await PromptAsync("添加工作站", "继续",
                Column(Field("设备名称", deviceName), Field("Linux 地址", address), Field("服务端口", port)), () =>
                {
                    draft = draft with { Name = deviceName.Text.Trim(), Host = address.Text.Trim(), Port = checked((int)port.Value) };
                    draft.Validate();
                });
            if (!foundAddress) return;

            var pin = new TextBox { PlaceholderText = "完整的 SHA-256 证书指纹" };
            var code = new PasswordBox { PlaceholderText = "一次性配对码" };
            bool hasTrust = await PromptAsync("确认服务端身份", "配对",
                Column(Heading($"{draft.Host}:{draft.Port}", 17),
                    Text("在 Linux 上运行 waybridge-service pair，或通过服务端管理界面生成配对码。请从可信渠道复制完整证书指纹。"),
                    Field("SHA-256 指纹（64 个十六进制字符）", pin), Field("一次性配对码", code)), () =>
                {
                    string normalized = pin.Text.Trim().ToLowerInvariant();
                    if (normalized.Length != 64 || normalized.Any(c => !Uri.IsHexDigit(c)))
                        throw new ArgumentException("请输入完整的 64 字符 SHA-256 指纹");
                    if (code.Password.Trim().Length == 0) throw new ArgumentException("请输入一次性配对码");
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
            Inform("已配对，凭据存入 Windows 凭据管理器。");
        });
    }

    private async Task<bool> PromptAsync(string title, string action, StackPanel content, Action validate)
    {
        if (activeDialog is not null) throw new InvalidOperationException("请先关闭当前对话框");
        var issue = new InfoBar { Severity = InfoBarSeverity.Error, IsClosable = false };
        content.Children.Add(issue);
        var dialog = new ContentDialog
        {
            XamlRoot = root.XamlRoot, RequestedTheme = root.RequestedTheme,
            Title = title, Content = content, DefaultButton = ContentDialogButton.Primary,
            PrimaryButtonText = action, CloseButtonText = "取消"
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
        if (!await ConfirmAsync("移除设备？", $"{profile.Name} 将从此客户端移除。可在服务端撤销对应授权。", "移除")) return;
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
        await SaveLauncherPreferencesAsync();
        if (remaining is not null) await RefreshApplicationsAsync();
    });

    private Task RevokeProfileAsync() => RunOperationAsync(async () =>
    {
        if (controller.IsRunning || !paired) return;
        var profile = selected;
        if (!await ConfirmAsync("撤销此设备配对？", "此客户端将失去对此 Linux 服务的访问权限。", "撤销配对")) return;
        string credential = secrets.Read(profile.Id) ?? throw new InvalidOperationException("此设备需要重新配对");
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await new DirectAgent(profile, credential).CallAsync(new { op = "revoke", revoke_client_id = profile.ClientId }, deadline.Token);
        secrets.Delete(profile.Id);
        await StoreProfileAsync(profile with { ClientId = "" });
        Inform("已撤销配对。可添加设备以重新配对。");
    });

    private Task ConfirmEndAsync() => RunOperationAsync(async () =>
    {
        var profileId = selected.Id;
        if (!controller.Connected || controller.ActiveProfileId != profileId) return;
        if (await ConfirmAsync("结束远端会话？", "这会关闭会话中的 Linux 应用。仅断开连接会保留应用。", "结束会话"))
            await controller.EndSessionAsync(expectedProfileId: profileId);
    });
}
