using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Waybridge.Control.Core;
using Windows.Storage.Pickers;

namespace Waybridge.Control;

internal sealed class MainWindow : Window
{
    private readonly Grid root = new();
    private readonly NavigationView navigation = new() { PaneDisplayMode = NavigationViewPaneDisplayMode.Auto, IsBackButtonVisible = NavigationViewBackButtonVisible.Collapsed, IsSettingsVisible = false };
    private readonly Dictionary<string, UIElement> pages = new();
    private readonly TextBlock status = new() { Text = "未连接", TextWrapping = TextWrapping.Wrap };
    private readonly InfoBar notice = new() { IsOpen = false, IsClosable = true };
    private readonly ProfileStore store;
    private readonly CredentialStore secrets = new();
    private readonly SessionController controller;
    private readonly DesktopNotifications desktopNotifications;
    private readonly TrayIcon tray;
    private readonly ObservableCollection<ConnectionProfile> profiles = [];
    private readonly ComboBox profilePicker = new() { DisplayMemberPath = "Name", MinWidth = 220, HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly TextBox name = new(), host = new(), user = new(), identity = new(), binary = new(), session = new(), udpHost = new();
    private readonly TextBox fingerprint = new(), clientId = new() { IsReadOnly = true };
    private readonly PasswordBox pairCode = new();
    private readonly Button pair = Button("配对此设备"), revoke = Button("撤销此设备配对");
    private readonly PasswordBox token = new();
    private readonly CheckBox remember = new() { Content = "使用 Windows 凭据管理器保存令牌" };
    private readonly ComboBox connectionMode = Combo(["认证加密直连", "内置 SSH", "已有 SSH 隧道"]);
    private readonly ComboBox sessionMode = Combo(["已有会话优先，未运行时启动", "仅接入已有会话", "创建独立会话"]);
    private readonly NumberBox sshPort = Number(22, 1, 65535), remotePort = Number(49090, 0, 65535);
    private readonly ToggleSwitch reconnect = Toggle(true), audio = Toggle(true), files = Toggle(true), dragText = Toggle(true), images = Toggle(true), readOnly = Toggle(false), ime = Toggle(true), udp = Toggle(false), hdr = Toggle(false);
    private readonly ToggleSwitch notifications = Toggle(true);
    private readonly ToggleSwitch touch = Toggle(false);
    private readonly TextBlock touchStatus = new() { TextWrapping = TextWrapping.Wrap, FontSize = 12 };
    private Guid? touchStatusProfile;
    private bool? touchActive, touchSupported, touchLiveSupported;
    private bool touchReadOnly;
    private readonly ToggleSwitch pen = Toggle(false), gamepads = Toggle(false), gamepadRumble = Toggle(true);
    private readonly NumberBox penPressureGamma = Number(1, .25, 4), gamepadDeadzone = Number(15, 0, 50);
    private readonly TextBlock penStatus = new() { TextWrapping = TextWrapping.Wrap, FontSize = 12 };
    private readonly TextBlock gamepadStatus = new() { TextWrapping = TextWrapping.Wrap, FontSize = 12 };
    private Guid? inputDeviceStatusProfile;
    private bool? penActive, penSupported, penNativeActive, penLiveSupported, penHardwareAvailable;
    private bool? gamepadActive, gamepadActiveLease, gamepadSupported, gamepadLiveSupported, rumbleSupported;
    private bool inputDeviceReadOnly;
    private string penError = "", gamepadError = "";
    private List<string>? connectedGamepads;
    private int? localGamepadCount;
    private sealed record TouchApplication(string Id, string Name);
    private readonly ComboBox touchApplication = new() { DisplayMemberPath = "Name", MinWidth = 220, PlaceholderText = "连接后选择应用" };
    private readonly ComboBox touchAppMode = Combo(["自动选择", "优先原生触控", "鼠标兼容（长按右键、拖选）"]);
    private Dictionary<string, string> touchModes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> touchApplicationNames = new(StringComparer.Ordinal);
    private bool loadingTouchApplication;
    private string? editingTouchApplication;
    private readonly ComboBox codec = Combo(["自动", "H.264", "HEVC", "AV1", "PNG 无损"]);
    private readonly ComboBox scale = Combo(["跟随 Windows", "100%", "125%", "150%", "175%", "200%", "250%", "300%", "400%"]);
    private readonly ComboBox clipboard = Combo(["双向同步", "仅 Windows → Linux", "仅 Linux → Windows", "关闭"]);
    private readonly ComboBox gpu = Combo(["CPU 兼容路径", "CUDA 颜色输入"]);
    private readonly ComboBox output = new() { MinWidth = 220 };
    private readonly NumberBox fps = Number(60, 1, 120), bitrate = Number(30, 1, 200);
    private readonly Slider volume = new() { Minimum = 0, Maximum = 100, Value = 100, StepFrequency = 1, MinWidth = 200 };
    private readonly Button connect = Button("连接", true), end = Button("结束远端会话"), save = Button("保存连接");
    private readonly Button addProfile = Button("新建"), deleteProfile = Button("删除");
    private string badgePng = "";
    private readonly Image badgePreview = new() { Width = 32, Height = 32 };
    private readonly TextBlock actual = new() { Text = "实际编码／解码／呈现：等待连接", TextWrapping = TextWrapping.Wrap };
    private readonly TextBox applicationSearch = new() { PlaceholderText = "搜索已安装应用" };
    private readonly ListView applicationList = new() { SelectionMode = ListViewSelectionMode.Single, MaxHeight = 430 };
    private readonly ListView windowList = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 280 };
    private readonly ToggleSwitch remoteTray = Toggle(true);
    private readonly TextBlock trayStatus = new() { Text = "托盘：等待连接", TextWrapping = TextWrapping.Wrap };
    private readonly ListView backgroundList = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 280 };
    private readonly ListView transfers = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 350 };
    private readonly TextBox logs = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 180, MaxHeight = 320 };
    private readonly Dictionary<string, TextBlock> metrics = new();
    private IReadOnlyList<RemoteApplication> applications = [];
    private ConnectionProfile selected = NewProfile();
    private ContentDialog? activeDialog;
    private bool loading, quitting, busy;
    private ulong lastBytes, lastUpdates, lastTick;
    private string receiveDirectory = "";
    private readonly IntPtr hwnd;
    private readonly List<(string Id, string Name)> audioDevices = [("", "跟随系统默认输出")];

    public MainWindow(string? stateDirectory = null)
    {
        Title = "Waybridge";
        hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var appWindow = AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(hwnd));
        appWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "waybridge.ico"));
        double density = GetDpiForWindow(hwnd) / 96.0;
        var work = DisplayArea.GetFromWindowId(appWindow.Id, DisplayAreaFallback.Nearest).WorkArea;
        appWindow.Resize(new Windows.Graphics.SizeInt32(Math.Min(work.Width - 40, (int)(1050*density)), Math.Min(work.Height - 40, (int)(780*density))));
        appWindow.Closing += (_, args) => { if (!quitting) { args.Cancel = true; TrayIcon.ShowWindow(hwnd, 0); } };
        try { SystemBackdrop = new MicaBackdrop(); } catch (COMException) { }
        string directory = stateDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Waybridge");
        store = new(directory);
        controller = new(directory, Path.Combine(AppContext.BaseDirectory, "waybridge-client.exe"), ConfirmHostAsync);
        desktopNotifications = new(request => controller.SendEngineAsync(request), message => UI(() => Inform(message)));
        controller.Status += text => UI(() => status.Text = text);
        controller.ConnectionChanged += connected => UI(() =>
        {
            desktopNotifications.ConnectionChanged(connected);
            TouchConnectionChanged(connected);
            if (!connected) { backgroundList.Items.Clear(); trayStatus.Text = "托盘：未连接"; }
            connect.Content = connected ? "断开连接" : (busy || controller.IsRunning) ? "取消连接" : "连接";
            end.IsEnabled = connected && selected.Mode is "ssh" or "direct";
            tray?.Update(connected);
            UpdateMode();
            if (!connected) { actual.Text = "实际编码／解码／呈现：等待连接"; lastTick = lastBytes = lastUpdates = 0; }
        });
        controller.Log += _ => UI(() => logs.Text = string.Join(Environment.NewLine, controller.Logs));
        controller.Message += message => { if (!desktopNotifications.Receive(message)) UI(() => Receive(message)); };
        tray = new(action => UI(() => TrayAction(action)));
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        notice.Margin = new Thickness(12, 8, 12, 0); root.Children.Add(notice);
        Grid.SetRow(navigation, 1); root.Children.Add(navigation);
        var footer = new Grid { Padding = new Thickness(18, 10, 18, 10), ColumnSpacing = 16 };
        footer.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        footer.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        footer.Children.Add(status);
        var architecture = Text($"{RuntimeInformation.ProcessArchitecture} · 关闭窗口时收起到托盘", 12);
        Grid.SetColumn(architecture, 1); footer.Children.Add(architecture); Grid.SetRow(footer, 2); root.Children.Add(footer);
        Content = root;
        AddPage("connection", "设备与连接", "\uE968", ConnectionPage());
        AddPage("apps", "远端应用", "\uE71D", ApplicationsPage());
        AddPage("display", "画面与声音", "\uE713", DisplayPage());
        AddPage("input", "输入与共享", "\uE765", InputPage());
        AddPage("files", "文件传输", "\uE8B7", TransfersPage());
        AddPage("diagnostics", "诊断", "\uE9D9", DiagnosticsPage());
        navigation.SelectionChanged += (_, args) =>
        {
            if (args.SelectedItem is NavigationViewItem item && item.Tag is string id)
            { navigation.Content = pages[id]; navigation.Header = item.Content; }
        };
        navigation.SelectedItem = navigation.MenuItems[0];
        root.Loaded += async (_, _) => await GuardAsync(InitializeAsync);
    }
    private void UI(Action action) => DispatcherQueue.TryEnqueue(() => { try { action(); } catch (Exception error) { Error(error); } });
    private void Error(Exception error) { notice.Severity = InfoBarSeverity.Error; notice.Title = "操作未完成"; notice.Message = error.Message; notice.IsOpen = true; }
    private void Inform(string message) { notice.Severity = InfoBarSeverity.Informational; notice.Title = ""; notice.Message = message; notice.IsOpen = true; }
    private async Task GuardAsync(Func<Task> action)
    {
        try { await action(); }
        catch (OperationCanceledException) { status.Text = "已取消"; }
        catch (Exception error) { Error(error); }
    }
    private static TextBlock Text(string value, double size = 14) => new() { Text = value, FontSize = size, TextWrapping = TextWrapping.Wrap };
    private static ComboBox Combo(string[] items)
    {
        var result = new ComboBox { MinWidth = 200, HorizontalAlignment = HorizontalAlignment.Stretch };
        foreach (string item in items) result.Items.Add(new ComboBoxItem { Content = item });
        result.SelectedIndex = 0; return result;
    }
    private static NumberBox Number(double value, double min, double max) => new()
    { Value = value, Minimum = min, Maximum = max, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, MinWidth = 150 };
    private static ToggleSwitch Toggle(bool value) => new() { IsOn = value, OnContent = "开启", OffContent = "关闭" };
    private static ConnectionProfile NewProfile() => new() { Mode = "direct", Port = 49092 };
    private static Button Button(string label, bool primary = false)
    {
        var result = new Button { Content = label, MinHeight = 36 };
        if (primary && Application.Current.Resources.TryGetValue("AccentButtonStyle", out var style)) result.Style = (Style)style;
        return result;
    }
    private static StackPanel Column(params UIElement[] children)
    {
        var panel = new StackPanel { Spacing = 12 };
        foreach (var child in children) panel.Children.Add(child);
        return panel;
    }
    private static StackPanel Actions(params UIElement[] children)
    {
        var result = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        foreach (var child in children) result.Children.Add(child);
        result.SizeChanged += (_, args) => result.Orientation = args.NewSize.Width < 560 ? Orientation.Vertical : Orientation.Horizontal;
        return result;
    }
    private static UIElement Row(string title, string description, FrameworkElement control)
    {
        var grid = new Grid { Padding = new Thickness(16, 13, 16, 13), ColumnSpacing = 20 };
        grid.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        grid.RowDefinitions.Add(new() { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new() { Height = GridLength.Auto });
        grid.Children.Add(Column(Text(title), Text(description, 12)));
        control.VerticalAlignment = VerticalAlignment.Center; control.MaxWidth = 360;
        Grid.SetColumn(control, 1); grid.Children.Add(control);
        bool compact = false;
        grid.SizeChanged += (_, args) =>
        {
            bool next = args.NewSize.Width < 530;
            if (next == compact) return;
            compact = next;
            Grid.SetColumn(control, compact ? 0 : 1); Grid.SetRow(control, compact ? 1 : 0);
            Grid.SetColumnSpan(control, compact ? 2 : 1);
            control.Margin = compact ? new Thickness(0, 10, 0, 0) : new Thickness(0);
        };
        var border = new Border { CornerRadius = new CornerRadius(8), Child = grid };
        if (Application.Current.Resources.TryGetValue("CardBackgroundFillColorDefaultBrush", out var brush)) border.Background = (Brush)brush;
        return border;
    }
    private static UIElement Field(string label, Microsoft.UI.Xaml.Controls.Control field)
    {
        field.HorizontalAlignment = HorizontalAlignment.Stretch; field.MaxWidth = double.PositiveInfinity;
        return Column(Text(label), field);
    }
    private void AddPage(string id, string title, string glyph, UIElement page)
    {
        pages[id] = new ScrollViewer { Content = page, Padding = new Thickness(24, 10, 24, 24), HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        navigation.MenuItems.Add(new NavigationViewItem
        { Content = title, Tag = id, Icon = new FontIcon { Glyph = glyph, FontFamily = new FontFamily("Segoe Fluent Icons") } });
    }
    private UIElement ConnectionPage()
    {
        profilePicker.ItemsSource = profiles;
        profilePicker.SelectionChanged += (_, _) => { if (!loading && profilePicker.SelectedItem is ConnectionProfile profile) Load(profile); };
        addProfile.Click += (_, _) => { var profile = NewProfile(); profiles.Add(profile); profilePicker.SelectedItem = profile; };
        deleteProfile.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (controller.Connected) throw new InvalidOperationException("删除连接前请先断开");
            secrets.Delete(selected.Id); profiles.Remove(selected);
            if (profiles.Count == 0) profiles.Add(NewProfile());
            profilePicker.SelectedIndex = 0; await store.SaveAsync(profiles);
        });
        connectionMode.SelectionChanged += (_, _) =>
        {
            if (!loading)
            {
                if (connectionMode.SelectedIndex == 0 && remotePort.Value == 49090) remotePort.Value = 49092;
                UpdateMode();
            }
        };
        name.PlaceholderText = "连接名称"; host.PlaceholderText = "Linux 主机名或 IP";
        user.PlaceholderText = "留空使用 SSH 配置中的用户"; identity.PlaceholderText = "留空使用 ssh-agent / OpenSSH 默认身份";
        binary.PlaceholderText = "waybridge 或 Linux 可执行文件完整路径";
        session.PlaceholderText = "waybridge-0"; token.PlaceholderText = "配对后自动填入；已有隧道模式填写会话令牌";
        fingerprint.PlaceholderText = "Linux 配对命令显示的 SHA-256 证书指纹";
        pairCode.PlaceholderText = "Linux 服务生成的一次性配对码";
        var help = Text("直连模式先核对 Linux 服务的证书指纹并配对，之后直接连接局域网或 VPN 地址。SSH 模式继续使用 Windows OpenSSH 与 ssh-agent。", 12);
        pair.Click += async (_, _) => await GuardAsync(async () =>
        {
            var profile = ReadEditor();
            if (profile.Mode != "direct" || controller.IsRunning) throw new InvalidOperationException("请先断开连接并选择直连模式");
            busy = true; connect.IsEnabled = false; UpdateMode();
            try
            {
                var credentials = await DirectAgent.PairAsync(profile, pairCode.Password, Environment.MachineName);
                clientId.Text = credentials.ClientId;
                token.Password = credentials.ClientToken;
                pairCode.Password = "";
                remember.IsChecked = true;
                await SaveAsync();
                Inform("配对成功，可以连接。客户端凭据已保存到 Windows 凭据管理器。");
            }
            finally { busy = false; connect.IsEnabled = true; UpdateMode(); }
        });
        revoke.Click += async (_, _) => await GuardAsync(async () =>
        {
            var profile = ReadEditor();
            if (profile.Mode != "direct" || controller.IsRunning) throw new InvalidOperationException("请先断开连接再撤销配对");
            busy = true; connect.IsEnabled = false; UpdateMode();
            try
            {
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
                await new DirectAgent(profile, token.Password).CallAsync(new { op = "revoke", revoke_client_id = profile.ClientId }, timeout.Token);
                secrets.Delete(profile.Id); clientId.Text = ""; token.Password = ""; remember.IsChecked = false;
                await SaveAsync(); Inform("此设备的配对已撤销。");
            }
            finally { busy = false; connect.IsEnabled = true; UpdateMode(); }
        });
        connect.Click += async (_, _) => await GuardAsync(ConnectOrDisconnectAsync);
        save.Click += async (_, _) => await GuardAsync(SaveAsync);
        end.IsEnabled = false;
        end.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (await ConfirmAsync("结束远端会话？", "这会关闭该会话中的 Linux 应用。仅断开连接则会保留应用。", "结束会话")) await controller.EndSessionAsync();
        });
        return Column(Actions(profilePicker, addProfile, deleteProfile), Field("连接名称", name), Field("连接方式", connectionMode),
            Field("主机地址", host), Row("SSH 端口", "内置 SSH 使用此端口", sshPort),
            Field("SSH 用户名", user), Field("私钥文件（可选）", identity), help,
            Field("Linux 服务端程序", binary), Field("会话名称", session),
            Row("会话策略", "直连与内置 SSH 都可创建或接入会话", sessionMode),
            Row("服务端口", "直连默认 49092；SSH 新建会话可填 0；已有隧道填本地转发端口", remotePort),
            Field("服务端证书指纹", fingerprint), Field("一次性配对码", pairCode), Actions(pair, revoke), Field("客户端标识", clientId),
            Field("客户端凭据／会话令牌", token), remember, Row("自动重连", "连接恢复后重新接入原会话，保留 Linux 应用", reconnect),
            Actions(connect, save, end));
    }
    private UIElement DisplayPage()
    {
        var apply = Button("应用到当前连接", true); apply.Click += async (_, _) => await GuardAsync(ApplyAsync);
        var restart = Button("断开并重新连接"); restart.Click += async (_, _) => await GuardAsync(async () => { await SaveAsync(); await controller.DisconnectAsync(); await ConnectOrDisconnectAsync(); });
        return Column(
            Row("视频编码", "实际编码由硬件能力与服务端共同决定", codec),
            Row("帧率上限", "实时应用，范围 1–120 FPS", fps),
            Row("每窗口视频码率（Mbps）", "实时应用；多个活动窗口会分别消耗带宽", bitrate),
            Row("应用内容缩放", "原生 Wayland 跟随每个 Windows 窗口；X11 共享应用密度", scale),
            Row("播放远端声音", "关闭时即时静音；首次启用音频可能需要重连", audio),
            Row("音量", "只影响当前远端会话", volume),
            Row("输出设备", "更换输出设备后重新连接", output),
            Row("UDP 媒体", "需要可直达的 Linux UDP 端口，不经过 SSH 转发；重连生效", udp),
            Field("UDP 主机（可选）", udpHost),
            Row("实验性 HDR", "需要服务端明确启用与兼容显示／解码器；重连生效", hdr),
            Row("服务端颜色输入", "只在创建新 Linux 会话时生效", gpu),
            actual, Actions(apply, restart));
    }
    private UIElement BadgeEditor()
    {
        var choose = Button("选择图片");
        var reset = Button("使用 Waybridge 图标");
        choose.Click += async (_, _) => await GuardAsync(async () =>
        {
            var picker = new FileOpenPicker();
            foreach (string extension in new[] { ".png", ".jpg", ".jpeg", ".bmp", ".ico" }) picker.FileTypeFilter.Add(extension);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var file = await picker.PickSingleFileAsync();
            if (file is null) return;
            badgePng = await IconBranding.NormalizeAsync(file);
            await UpdateBadgePreviewAsync();
        });
        reset.Click += async (_, _) => { badgePng = ""; await GuardAsync(UpdateBadgePreviewAsync); };
        return Column(Row("远端图标角标", "保留应用图标，在右上角标记来源；随当前连接保存，可实时应用", badgePreview), Actions(choose, reset));
    }
    private async Task UpdateBadgePreviewAsync()
    {
        string current = badgePng;
        var preview = await IconBranding.PreviewAsync(current);
        if (current == badgePng) badgePreview.Source = preview;
    }
    private UIElement InputPage()
    {
        var saveSettings = Button("保存设置"); saveSettings.Click += async (_, _) => await GuardAsync(SaveAsync);
        var apply = Button("应用到当前连接", true); apply.Click += async (_, _) => await GuardAsync(ApplyAsync);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touch, "NativeTouchToggle");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(touch, "原生多点触控");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchStatus, "NativeTouchStatus");
        touch.Toggled += (_, _) => { if (!loading) UpdateTouchStatus(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchApplication, "TouchApplication");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchAppMode, "TouchApplicationMode");
        touchApplication.SelectionChanged += (_, _) => LoadTouchApplicationMode();
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(saveSettings, "InputSettingsSave");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(apply, "InputSettingsApply");
        foreach (var (control, id, label) in new[] { (pen, "NativePenToggle", "压感手写笔"),
                     (gamepads, "GamepadToggle", "游戏手柄"), (gamepadRumble, "GamepadRumbleToggle", "手柄震动") })
        {
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(control, id);
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(control, label);
            control.Toggled += (_, _) => { if (!loading) UpdateInputDeviceStatus(); };
        }
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(penPressureGamma, "PenPressureGamma");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(penPressureGamma, "笔压力度曲线");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(gamepadDeadzone, "GamepadDeadzone");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(gamepadDeadzone, "摇杆死区百分比");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(penStatus, "NativePenStatus");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(gamepadStatus, "GamepadStatus");
        penPressureGamma.SmallChange = .25;
        penPressureGamma.ValueChanged += (_, _) => { if (!loading) UpdateInputDeviceStatus(); };
        gamepadDeadzone.ValueChanged += (_, _) => { if (!loading) UpdateInputDeviceStatus(); };
        return Column(BadgeEditor(), Row("剪贴板方向", "可分别限制 Windows 与 Linux 间的数据同步", clipboard),
            Row("剪贴板图片", "文本同步保持独立", images),
            Row("文字拖放", "双向文字拖放，独立于剪贴板与文件共享", dragText),
            Row("文件与目录共享", "控制文件与目录的剪贴板、拖入与拖出", files),
            Row("桌面通知", "Linux 应用通知与动作，遵循 Windows 通知设置及免打扰", notifications),
            Row("远端应用托盘", "在 Windows 通知区域显示应用托盘，关闭主窗口后仍可从后台列表恢复", remoteTray),
            Row("只读模式", "暂停远端输入，释放按键、鼠标锁定、手写笔和手柄", readOnly),
            Row("Windows 本地 IME", "使用本地候选窗口与远端预编辑", ime),
            Row("原生多点触控（实验）", "向支持触控的 Linux 应用发送最多十点触摸；关闭时使用鼠标兼容模式。按连接保存，可立即应用。", touch),
            touchStatus,
            Row("应用触控模式", "默认自动选择：应用接入触控时使用原生输入，否则使用鼠标兼容。可为有兼容问题的应用单独覆盖，随连接保存。", touchApplication),
            Row("所选应用的输入方式", "通常保留自动选择。原生触控的长按与选字由应用处理，鼠标兼容支持系统长按右键和拖选。", touchAppMode),
            Row("压感手写笔（实验）", "向支持手写笔的绘图应用发送压力、倾斜和橡皮擦输入；按连接保存，可立即应用。", pen),
            Row("笔压力度曲线", "1 为线性；小于 1 时轻压更明显，大于 1 时需要更用力。范围 0.25–4。", penPressureGamma),
            penStatus,
            Row("游戏手柄（实验）", "将 Windows 连接的手柄提供给当前 Linux 会话；只在开启此设置后转发。", gamepads),
            Row("手柄震动", "允许游戏向当前连接的手柄发送震动反馈", gamepadRumble),
            Row("摇杆死区（%）", "忽略摇杆中心附近的小幅偏移，减少漂移；范围 0%–50%。", gamepadDeadzone),
            gamepadStatus,
            Row("紧急释放鼠标", "游戏捕获鼠标时可随时释放", Text("Ctrl + Alt + Shift + F12")), Actions(saveSettings, apply));
    }
    private UIElement ApplicationsPage()
    {
        var refresh = Button("刷新应用"); refresh.Click += async (_, _) => await GuardAsync(RefreshApplicationsAsync);
        var terminal = Button("启动终端", true); terminal.Click += async (_, _) => await GuardAsync(() => controller.LaunchAsync(argv: ["ghostty"]));
        var launch = Button("启动选中应用"); launch.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (applicationList.SelectedItem is ListViewItem { Tag: RemoteApplication app }) await controller.LaunchAsync(app: app.Id);
        });
        applicationSearch.TextChanged += (_, _) => FilterApplications();
        return Column(Text("从 Linux 已安装应用中选择，启动到当前私有会话。"), Actions(refresh, terminal, launch),
            applicationSearch, applicationList, Text("当前远端窗口", 18), windowList,
            Text("后台应用", 18), trayStatus, backgroundList);
    }
    private UIElement TransfersPage()
    {
        var send = Button("发送文件", true);
        var folder = Button("发送目录");
        var receive = Button("打开只读接收文件夹");
        send.Click += async (_, _) => await GuardAsync(async () =>
        {
            var picker = new FileOpenPicker(); picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickMultipleFilesAsync();
            if (picked.Count > 0) await controller.SendEngineAsync(new { type = "send_files", paths = picked.Select(p => p.Path).ToArray() });
        });
        folder.Click += async (_, _) => await GuardAsync(async () =>
        {
            var picker = new FolderPicker(); picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickSingleFolderAsync();
            if (picked is not null) await controller.SendEngineAsync(new { type = "send_files", paths = new[] { picked.Path } });
        });
        receive.Click += async (_, _) => await GuardAsync(() =>
        {
            string path = receiveDirectory;
            if (!Directory.Exists(path)) { Inform("尚无已接收文件，或接收目录已被移除。"); return Task.CompletedTask; }
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            return Task.CompletedTask;
        });
        return Column(Text("也可以直接在 Windows 与远端窗口之间拖放文件或目录。接收项为只读输入；需要编辑并保存时，先复制到自己选择的文件夹。"), Actions(send, folder, receive), transfers);
    }
    private UIElement DiagnosticsPage()
    {
        var content = new StackPanel { Spacing = 10 };
        foreach (var (key, label) in new[] { ("rtt", "网络往返 RTT"), ("rate", "TCP 接收速率"), ("updates", "窗口呈现更新（合计）"),
            ("decode_queue","解码排队"), ("decode","视频解码"), ("ui_queue","UI 排队"), ("present","呈现提交") })
        { var value = Text("等待采集"); metrics[key] = value; content.Children.Add(Row(label, "", value)); }
        content.Children.Add(Text("RTT、排队、解码与呈现分别计时，不等于输入到显示的总延迟。", 12));
        var export = Button("导出诊断"); export.Click += async (_, _) => await GuardAsync(async () =>
        {
            var picker = new FileSavePicker { SuggestedFileName = "waybridge-diagnostics" };
            picker.FileTypeChoices.Add("文本日志", [".txt"]); WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickSaveFileAsync();
            if (picked is not null)
            {
                string text = $"Waybridge Control\nGUI architecture: {RuntimeInformation.ProcessArchitecture}\n{actual.Text}\n" +
                    string.Join('\n', metrics.Select(pair => pair.Key + ": " + pair.Value.Text)) + "\n" + string.Join('\n', controller.Logs);
                await File.WriteAllTextAsync(picked.Path, text); Inform("诊断已导出，不包含会话令牌与剪贴板内容。");
            }
        });
        content.Children.Add(export); content.Children.Add(logs);
        return content;
    }
    private async Task InitializeAsync()
    {
        var saved = await store.LoadAsync();
        loading = true;
        foreach (var profile in saved) profiles.Add(profile);
        if (profiles.Count == 0) profiles.Add(NewProfile());
        loading = false; profilePicker.SelectedIndex = 0;
        try
        {
            var found = await Windows.Devices.Enumeration.DeviceInformation.FindAllAsync(Windows.Media.Devices.MediaDevice.GetAudioRenderSelector());
            foreach (var device in found) audioDevices.Add((device.Id, device.Name));
        }
        catch (Exception error) { Inform("音频设备列表不可用：" + error.Message); }
        output.ItemsSource = audioDevices.Select(d => d.Name).ToList(); Load(selected);
    }
    private void UpdateMode()
    {
        bool direct = connectionMode.SelectedIndex == 0, ssh = connectionMode.SelectedIndex == 1;
        bool editable = !busy && !controller.IsRunning;
        addProfile.IsEnabled = deleteProfile.IsEnabled = editable;
        reconnect.IsEnabled = editable;
        profilePicker.IsEnabled = name.IsEnabled = host.IsEnabled = connectionMode.IsEnabled = remotePort.IsEnabled = editable;
        user.IsEnabled = identity.IsEnabled = binary.IsEnabled = sshPort.IsEnabled = ssh && editable;
        session.IsEnabled = sessionMode.IsEnabled = (ssh || direct) && editable;
        fingerprint.IsEnabled = pairCode.IsEnabled = pair.IsEnabled = revoke.IsEnabled = clientId.IsEnabled = direct && editable;
        token.IsEnabled = remember.IsEnabled = !ssh && editable;
    }
    private void Load(ConnectionProfile profile)
    {
        if (selected.Id != profile.Id) { ResetTouchStatus(); ResetInputDeviceStatus(); touchApplicationNames.Clear(); }
        loading = true; selected = profile;
        name.Text = profile.Name; host.Text = profile.Host; user.Text = profile.User; identity.Text = profile.IdentityFile;
        binary.Text = profile.ServerBinary; session.Text = profile.Session; sshPort.Value = profile.SshPort; remotePort.Value = profile.Port;
        connectionMode.SelectedIndex = profile.Mode switch { "direct" => 0, "ssh" => 1, _ => 2 };
        fingerprint.Text = profile.CertificateSha256; clientId.Text = profile.ClientId; pairCode.Password = "";
        sessionMode.SelectedIndex = Array.IndexOf(new[] { "auto", "attach", "new" }, profile.SessionMode);
        reconnect.IsOn = profile.AutoReconnect;
        token.Password = secrets.Read(profile.Id) ?? ""; remember.IsChecked = token.Password.Length > 0;
        var settings = profile.Settings;
        touchModes = new(settings.TouchModes, StringComparer.Ordinal);
        RefreshTouchApplications();
        badgePng = settings.BadgePng;
        _ = GuardAsync(UpdateBadgePreviewAsync);
        codec.SelectedIndex = Array.IndexOf(new[] { "auto", "h264", "hevc", "av1", "png" }, settings.Codec);
        fps.Value = settings.Fps; bitrate.Value = settings.Bitrate / 1000.0;
        scale.SelectedIndex = Array.IndexOf(new[] { 0, 96, 120, 144, 168, 192, 240, 288, 384 }, settings.Dpi);
        if (scale.SelectedIndex < 0) scale.SelectedIndex = 0;
        audio.IsOn = settings.Audio; volume.Value = settings.Volume;
        output.SelectedIndex = Math.Max(0, audioDevices.FindIndex(d => d.Id == settings.AudioDevice));
        udp.IsOn = settings.Udp; udpHost.Text = settings.UdpHost; hdr.IsOn = settings.Hdr;
        gpu.SelectedIndex = settings.GpuInput == "cuda" ? 1 : 0;
        clipboard.SelectedIndex = Array.IndexOf(new[] { "both", "send", "receive", "off" }, settings.Clipboard);
        remoteTray.IsOn = settings.Tray;
        images.IsOn = settings.ClipboardImages; files.IsOn = settings.Files; dragText.IsOn = settings.DragText; readOnly.IsOn = settings.ReadOnly; ime.IsOn = settings.Ime; touch.IsOn = settings.Touch;
        pen.IsOn = settings.Pen; penPressureGamma.Value = settings.PenPressureGamma;
        gamepads.IsOn = settings.Gamepads; gamepadRumble.IsOn = settings.GamepadRumble; gamepadDeadzone.Value = settings.GamepadDeadzone;
        notifications.IsOn = settings.Notifications;
        desktopNotifications.SharingChanged(settings.Notifications);
        loading = false; UpdateMode(); UpdateTouchStatus(); UpdateInputDeviceStatus();
    }
    private ConnectionProfile ReadEditor()
    {
        CommitTouchApplicationMode();
        var settings = new ClientSettings
        {
            Codec = new[] { "auto", "h264", "hevc", "av1", "png" }[Math.Max(0, codec.SelectedIndex)],
            Fps = checked((int)fps.Value), Bitrate = checked((int)(bitrate.Value * 1000)),
            Dpi = new[] { 0, 96, 120, 144, 168, 192, 240, 288, 384 }[Math.Max(0, scale.SelectedIndex)],
            Audio = audio.IsOn, Volume = (int)volume.Value, AudioDevice = audioDevices[Math.Clamp(output.SelectedIndex, 0, audioDevices.Count - 1)].Id,
            Hdr = hdr.IsOn, Udp = udp.IsOn, UdpHost = udpHost.Text.Trim(), GpuInput = gpu.SelectedIndex == 1 ? "cuda" : "cpu",
            Clipboard = new[] { "both", "send", "receive", "off" }[Math.Max(0, clipboard.SelectedIndex)],
            BadgePng = badgePng, ClipboardImages = images.IsOn, Files = files.IsOn, DragText = dragText.IsOn, ReadOnly = readOnly.IsOn, Ime = ime.IsOn, Touch = touch.IsOn,
            Pen = pen.IsOn, PenPressureGamma = penPressureGamma.Value,
            Gamepads = gamepads.IsOn, GamepadRumble = gamepadRumble.IsOn, GamepadDeadzone = checked((int)gamepadDeadzone.Value),
            Notifications = notifications.IsOn, Tray = remoteTray.IsOn, TouchModes = new(touchModes, StringComparer.Ordinal)
        };
        var result = selected with
        {
            Name = name.Text.Trim(), Host = host.Text.Trim(), User = user.Text.Trim(), IdentityFile = identity.Text.Trim(),
            ServerBinary = binary.Text.Trim(), Session = session.Text.Trim(), SshPort = checked((int)sshPort.Value), Port = checked((int)remotePort.Value),
            Mode = connectionMode.SelectedIndex switch { 0 => "direct", 1 => "ssh", _ => "tunnel" },
            CertificateSha256 = fingerprint.Text.Trim().ToLowerInvariant(), ClientId = clientId.Text.Trim(),
            SessionMode = new[] { "auto", "attach", "new" }[Math.Max(0, sessionMode.SelectedIndex)],
            AutoReconnect = reconnect.IsOn, Settings = settings
        };
        result.Validate(); return result;
    }
    private async Task SaveAsync()
    {
        var profile = ReadEditor();
        if (profile.Mode is "tunnel" or "direct" && remember.IsChecked == true && token.Password.Length > 0) secrets.Write(profile.Id, token.Password);
        else secrets.Delete(profile.Id);
        int index = profiles.ToList().FindIndex(saved => saved.Id == profile.Id);
        if (index < 0) throw new InvalidOperationException("当前连接配置已不存在，请重新选择连接");
        loading = true; profiles[index] = profile; profilePicker.SelectedIndex = index; loading = false;
        selected = profile; await store.SaveAsync(profiles); Inform("连接设置已保存。"); UpdateTouchStatus(); UpdateInputDeviceStatus();
    }
    private async Task ConnectOrDisconnectAsync()
    {
        if (controller.IsRunning || busy)
        {
            await controller.DisconnectAsync(); busy = false; connect.Content = "连接"; return;
        }
        var profile = ReadEditor();
        busy = true; connect.Content = "取消连接"; notice.IsOpen = false; UpdateMode();
        desktopNotifications.SharingChanged(profile.Settings.Notifications);
        try
        {
            await controller.ConnectAsync(profile, token.Password); connect.Content = "断开连接";
            if (profile.Mode is "ssh" or "direct" && controller.CurrentSession is { } actualSession)
            {
                session.Text = actualSession; sessionMode.SelectedIndex = 0;
                await SaveAsync();
            }
            await RefreshApplicationsAsync();
        }
        finally { busy = false; if (!controller.Connected) connect.Content = "连接"; UpdateMode(); }
    }
    private async Task ApplyAsync()
    {
        var profile = ReadEditor();
        if (controller.ActiveProfileId != profile.Id) throw new InvalidOperationException("当前连接属于另一个配置，请先选择该连接");
        await controller.ApplyAsync(profile.Settings); await SaveAsync();
        desktopNotifications.SharingChanged(profile.Settings.Notifications);
    }
    private async Task RefreshApplicationsAsync()
    {
        applications = await controller.ApplicationsAsync(); FilterApplications();
        if (applications.Count == 0) Inform("应用目录需要直连或内置 SSH 会话；已有隧道模式仍可在远端终端启动应用。");
    }
    private void FilterApplications()
    {
        applicationList.Items.Clear();
        foreach (var app in applications.Where(a => a.Name.Contains(applicationSearch.Text, StringComparison.OrdinalIgnoreCase)))
            applicationList.Items.Add(new ListViewItem { Content = Text(app.Name), Tag = app });
    }
    private void UpdateBackground(JsonElement items)
    {
        backgroundList.Items.Clear();
        foreach (var item in items.EnumerateArray())
        {
            string id = item.GetProperty("id").GetString() ?? "";
            ulong epoch = item.GetProperty("epoch").GetUInt64(), generation = item.GetProperty("generation").GetUInt64();
            string title = item.GetProperty("title").GetString() ?? "";
            if (title.Length == 0) title = item.GetProperty("app_id").GetString() ?? id;
            string state = item.TryGetProperty("status", out var value) ? value.GetString() ?? "" : "";
            var restore = Button("打开／恢复");
            restore.IsEnabled = !readOnly.IsOn;
            restore.Click += async (_, _) => await GuardAsync(() => controller.SendEngineAsync(new { type = "tray_action", id, epoch, generation, action = "activate" }));
            var menu = Button("应用菜单");
            menu.IsEnabled = !readOnly.IsOn;
            menu.Click += async (_, _) => await GuardAsync(() => controller.SendEngineAsync(new { type = "tray_action", id, epoch, generation, action = "menu" }));
            backgroundList.Items.Add(Row(title, state == "NeedsAttention" ? "应用需要注意" : "通过应用菜单正常退出", Actions(restore, menu)));
        }
    }
    private void RefreshTouchApplications()
    {
        string? previous = (touchApplication.SelectedItem as TouchApplication)?.Id;
        var entries = touchApplicationNames.Keys.Concat(touchModes.Keys).Distinct(StringComparer.Ordinal)
            .OrderBy(id => touchApplicationNames.GetValueOrDefault(id, id), StringComparer.CurrentCulture)
            .Select(id => new TouchApplication(id, touchApplicationNames.GetValueOrDefault(id, id))).ToList();
        if (touchApplication.ItemsSource is List<TouchApplication> current && current.SequenceEqual(entries))
        {
            if (loading) LoadTouchApplicationMode();
            return;
        }
        loadingTouchApplication = true;
        touchApplication.ItemsSource = entries;
        touchApplication.SelectedItem = entries.FirstOrDefault(e => e.Id == previous) ?? entries.FirstOrDefault();
        loadingTouchApplication = false;
        LoadTouchApplicationMode();
    }
    private void CommitTouchApplicationMode()
    {
        if (loading || loadingTouchApplication || editingTouchApplication is not { } id) return;
        if (touchAppMode.SelectedIndex <= 0) touchModes.Remove(id);
        else touchModes[id] = touchAppMode.SelectedIndex == 1 ? "native" : "mouse";
    }
    private void LoadTouchApplicationMode()
    {
        if (loadingTouchApplication) return;
        CommitTouchApplicationMode();
        loadingTouchApplication = true;
        editingTouchApplication = (touchApplication.SelectedItem as TouchApplication)?.Id;
        touchAppMode.IsEnabled = touchApplication.SelectedItem is TouchApplication;
        touchAppMode.SelectedIndex = touchApplication.SelectedItem is TouchApplication app
            ? touchModes.GetValueOrDefault(app.Id) switch { "native" => 1, "mouse" => 2, _ => 0 } : 0;
        loadingTouchApplication = false;
    }
    private static bool? TouchFlag(JsonElement message, string name) =>
        message.TryGetProperty(name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean() : null;
    private void ResetTouchStatus()
    {
        touchStatusProfile = null; touchActive = touchSupported = touchLiveSupported = null; touchReadOnly = false;
    }
    private void TouchConnectionChanged(bool connected)
    {
        if (!connected) { ResetTouchStatus(); ResetInputDeviceStatus(); }
        else
        {
            var profile = controller.ActiveProfileId ?? selected.Id;
            if (touchStatusProfile != profile)
            {
                ResetTouchStatus(); touchStatusProfile = profile; touchReadOnly = readOnly.IsOn;
            }
            if (inputDeviceStatusProfile != profile)
            {
                ResetInputDeviceStatus(); inputDeviceStatusProfile = profile; inputDeviceReadOnly = readOnly.IsOn;
            }
        }
        UpdateTouchStatus(); UpdateInputDeviceStatus();
    }
    private void ReceiveTouchStatus(JsonElement message)
    {
        if (TouchFlag(message, "touch_enabled") is not { } enabled) return;
        var profile = controller.ActiveProfileId ?? selected.Id;
        if (touchStatusProfile != profile) ResetTouchStatus();
        touchStatusProfile = profile; touchActive = enabled;
        touchSupported = TouchFlag(message, "touch_supported") ?? touchSupported;
        touchLiveSupported = TouchFlag(message, "touch_live_supported") ?? touchLiveSupported;
        if (message.TryGetProperty("settings", out var settings) && settings.ValueKind == JsonValueKind.Object)
            touchReadOnly = TouchFlag(settings, "read_only") ?? touchReadOnly;
        UpdateTouchStatus();
    }
    private void UpdateTouchStatus()
    {
        bool saved = selected.Settings.Touch;
        string text = $"设置：{(saved ? "开启" : "关闭")}。";
        if (touchStatusProfile != selected.Id) text += "当前未连接，连接后使用此设置。";
        else if (touchActive is not { } active) text += "正在确认当前连接的触控状态。";
        else
        {
            text += $"当前连接原生触控已{(active ? "开启" : "关闭")}。";
            if (saved && touchSupported == false) text += "Linux 端暂不支持原生触控，请更新后重连。";
            else if (saved != active) text += touchLiveSupported == false
                ? "已保存，重连后生效。" : "已保存，应用到当前连接后生效。";
            if (touchReadOnly) text += "只读模式已暂停远端输入。";
        }
        if (touch.IsOn != saved) text += "当前触控编辑尚未保存。";
        touchStatus.Text = text;
    }
    private void ResetInputDeviceStatus()
    {
        inputDeviceStatusProfile = null;
        penActive = penSupported = penNativeActive = penLiveSupported = penHardwareAvailable = null;
        gamepadActive = gamepadActiveLease = gamepadSupported = gamepadLiveSupported = rumbleSupported = null;
        inputDeviceReadOnly = false; penError = gamepadError = "";
        connectedGamepads = null; localGamepadCount = null;
    }
    private void ReceiveInputDeviceStatus(JsonElement message)
    {
        if (!message.TryGetProperty("pen_enabled", out _) && !message.TryGetProperty("pen_supported", out _) &&
            !message.TryGetProperty("gamepad_enabled", out _) && !message.TryGetProperty("gamepad_supported", out _) &&
            !message.TryGetProperty("pen_error", out _) && !message.TryGetProperty("gamepad_error", out _)) return;
        var profile = controller.ActiveProfileId ?? selected.Id;
        if (inputDeviceStatusProfile != profile) ResetInputDeviceStatus();
        inputDeviceStatusProfile = profile;
        penActive = TouchFlag(message, "pen_enabled") ?? penActive;
        penSupported = TouchFlag(message, "pen_supported") ?? penSupported;
        penNativeActive = TouchFlag(message, "pen_native_active") ?? penNativeActive;
        penLiveSupported = TouchFlag(message, "pen_live_supported") ?? penLiveSupported;
        penHardwareAvailable = TouchFlag(message, "pen_hardware_available") ?? penHardwareAvailable;
        gamepadActive = TouchFlag(message, "gamepad_enabled") ?? gamepadActive;
        gamepadActiveLease = TouchFlag(message, "gamepad_active") ?? gamepadActiveLease;
        gamepadSupported = TouchFlag(message, "gamepad_supported") ?? gamepadSupported;
        gamepadLiveSupported = TouchFlag(message, "gamepad_live_supported") ?? gamepadLiveSupported;
        rumbleSupported = TouchFlag(message, "rumble_supported") ?? rumbleSupported;
        if (message.TryGetProperty("pen_error", out var penFailure) && penFailure.ValueKind == JsonValueKind.String)
            penError = penFailure.GetString() ?? "";
        if (message.TryGetProperty("gamepad_error", out var gamepadFailure) && gamepadFailure.ValueKind == JsonValueKind.String)
            gamepadError = gamepadFailure.GetString() ?? "";
        if (message.TryGetProperty("gamepad_local_count", out var local))
            localGamepadCount = local.ValueKind == JsonValueKind.Number && local.TryGetInt32(out int count) && count is >= 0 and <= 4 ? count : null;
        if (message.TryGetProperty("gamepad_devices", out var devices) && devices.ValueKind == JsonValueKind.Array)
        {
            connectedGamepads = [];
            foreach (var device in devices.EnumerateArray())
            {
                if (device.ValueKind != JsonValueKind.Object || TouchFlag(device, "connected") == false) continue;
                string label = device.TryGetProperty("name", out var deviceName) && deviceName.ValueKind == JsonValueKind.String
                    ? deviceName.GetString() ?? "" : "";
                if (string.IsNullOrWhiteSpace(label))
                {
                    int slot = device.TryGetProperty("slot", out var slotValue) && slotValue.TryGetInt32(out var parsedSlot)
                        ? parsedSlot : device.TryGetProperty("userindex", out var userIndex) && userIndex.TryGetInt32(out var parsedIndex) ? parsedIndex : connectedGamepads.Count;
                    label = $"手柄 {slot + 1}";
                }
                connectedGamepads.Add(label);
            }
        }
        if (message.TryGetProperty("settings", out var settings) && settings.ValueKind == JsonValueKind.Object)
            inputDeviceReadOnly = TouchFlag(settings, "read_only") ?? inputDeviceReadOnly;
        inputDeviceReadOnly = TouchFlag(message, "read_only") ?? inputDeviceReadOnly;
        UpdateInputDeviceStatus();
    }
    private static string InputDeviceState(bool saved, bool? active, bool? supported, bool? live, string name)
    {
        string text = $"设置：{(saved ? "开启" : "关闭")}。";
        if (active is not { } enabled) return text + "正在确认当前连接的设备状态。";
        text += $"当前连接{name}已{(enabled ? "开启" : "关闭")}。";
        if (saved && supported == false) text += $"当前连接暂不支持{name}，请更新后重连。";
        else if (saved != enabled) text += live == false ? "已保存，重连后生效。" : "已保存，应用到当前连接后生效。";
        return text;
    }
    private void UpdateInputDeviceStatus()
    {
        bool current = inputDeviceStatusProfile == selected.Id;
        string off = "当前未连接，连接后使用此设置。";
        string penText = current ? InputDeviceState(selected.Settings.Pen, penActive, penSupported, penLiveSupported, "手写笔")
            : $"设置：{(selected.Settings.Pen ? "开启" : "关闭")}。" + off;
        string gamepadText = current ? InputDeviceState(selected.Settings.Gamepads, gamepadActive, gamepadSupported, gamepadLiveSupported, "手柄转发")
            : $"设置：{(selected.Settings.Gamepads ? "开启" : "关闭")}。" + off;
        if (current)
        {
            if (penActive == true && penSupported != false)
            {
                if (penHardwareAvailable == false) penText += "Windows 未检测到可用的手写笔设备。";
                else if (penNativeActive == true) penText += "当前应用已接入压感手写笔。";
                else if (penNativeActive == false) penText += "等待支持手写笔的应用接入。";
            }
            if (gamepadActive == true)
            {
                if (localGamepadCount == 0) gamepadText += "Windows 未检测到已连接的 XInput 手柄。请检查蓝牙或 USB 连接。";
                else if (localGamepadCount is int count) gamepadText += $"Windows 已识别 {count} 个手柄。";
                if (connectedGamepads?.Count > 0)
                    gamepadText += "远端设备：" + string.Join("、", connectedGamepads) + "。";
                else if (gamepadSupported == true && localGamepadCount > 0 && !inputDeviceReadOnly)
                    gamepadText += gamepadActiveLease == false ? "请先聚焦远端窗口。" : "正在等待 Linux 创建设备。";
            }
            if (gamepadActive == true && selected.Settings.GamepadRumble && rumbleSupported == false)
                gamepadText += "当前连接不支持震动反馈。";
            if (penError.Length > 0) penText += "手写笔暂不可用：" + penError;
            if (gamepadError.Length > 0) gamepadText += "手柄暂不可用：" + gamepadError;
            if (inputDeviceReadOnly) { penText += "只读模式已暂停远端输入。"; gamepadText += "只读模式已暂停远端输入。"; }
        }
        if (pen.IsOn != selected.Settings.Pen || penPressureGamma.Value != selected.Settings.PenPressureGamma) penText += "当前手写笔编辑尚未保存。";
        if (gamepads.IsOn != selected.Settings.Gamepads || gamepadRumble.IsOn != selected.Settings.GamepadRumble ||
            gamepadDeadzone.Value != selected.Settings.GamepadDeadzone) gamepadText += "当前手柄编辑尚未保存。";
        penStatus.Text = penText; gamepadStatus.Text = gamepadText;
    }
    private void Receive(JsonElement message)
    {
        if (desktopNotifications.Receive(message)) return;
        string? type = message.GetProperty("type").GetString();
        if (type is "status" or "local_settings_applied") ReceiveTouchStatus(message);
        if (type is "status" or "local_settings_applied" or "gamepad_state") ReceiveInputDeviceStatus(message);
        if (type == "notification_status") { Inform(message.GetProperty("message").GetString() ?? "通知服务状态已更新"); return; }
        if (type == "connected" && message.TryGetProperty("hardware_codecs", out var supported))
        {
            var capabilities = supported.EnumerateArray().Select(v => v.GetInt32()).ToHashSet();
            foreach (var (index, kind) in new[] { (1, 2), (2, 4), (3, 5) })
                ((ComboBoxItem)codec.Items[index]).IsEnabled = capabilities.Contains(kind);
        }
        else if (type == "tray_status")
        {
            trayStatus.Text = message.GetProperty("available").GetBoolean()
                ? "SNI 托盘已连接；旧式 XEmbed 托盘尚不支持。"
                : "托盘不可用：" + (message.TryGetProperty("message", out var reason) ? reason.GetString() : "未知原因");
        }
        else if (type is "tray_error" or "tray_warning" or "tray_info") Inform(message.GetProperty("message").GetString() ?? "托盘操作未完成");
        else if (type == "tray_background") UpdateBackground(message.GetProperty("items"));
        else if (type == "stream_config")
        {
            if (message.GetProperty("ok").GetBoolean())
            {
                touchReadOnly = TouchFlag(message, "read_only") ?? touchReadOnly;
                inputDeviceReadOnly = TouchFlag(message, "read_only") ?? inputDeviceReadOnly;
                UpdateTouchStatus(); UpdateInputDeviceStatus();
                string reason = message.TryGetProperty("fallback_reason", out var fallback) && fallback.GetString() == "session_video_disabled"
                    ? " 当前会话启动时限定 PNG；请新建使用视频编码的会话。" : "";
                Inform($"服务端已应用：{message.GetProperty("fps")} FPS，{message.GetProperty("bitrate").GetInt32()/1000.0:g} Mbps/窗口，{message.GetProperty("codec")}。实际画面路径见诊断。" + reason);
            }
            else Inform("服务端未接受设置：" + message.GetProperty("message").GetString());
        }
        else if (type is "error" or "settings_warning") Inform(message.GetProperty("message").GetString() ?? "操作失败");
        else if (type == "timings")
        {
            var stages = message.GetProperty("stages");
            foreach (var pair in new[] { ("control_rtt","rtt"), ("decode_queue","decode_queue"), ("ui_queue","ui_queue"), ("present","present") })
                if (stages.TryGetProperty(pair.Item1, out var value)) metrics[pair.Item2].Text = $"{value.GetProperty("avg_ms").GetDouble():F2} ms 平均 / {value.GetProperty("max_ms").GetDouble():F2} ms 最大";
            foreach (var stage in stages.EnumerateObject())
                if (stage.Name.EndsWith("_decode", StringComparison.Ordinal) && stage.Name != "alpha_decode")
                    metrics["decode"].Text = $"{stage.Value.GetProperty("avg_ms").GetDouble():F2} ms · {stage.Name}";
        }
        else if (type == "status")
        {
            ulong tick = message.GetProperty("uptime_ms").GetUInt64(), bytes = message.GetProperty("tcp_bytes").GetUInt64(), updates = message.GetProperty("presented_updates").GetUInt64();
            if (lastTick > 0 && tick > lastTick && bytes >= lastBytes && updates >= lastUpdates)
            {
                metrics["rate"].Text = $"{(bytes-lastBytes)*8.0/(tick-lastTick)/1000:F2} Mbps";
                metrics["updates"].Text = $"{(updates-lastUpdates)*1000.0/(tick-lastTick):F1} 次/秒";
            }
            lastTick = tick; lastBytes = bytes; lastUpdates = updates;
            if (message.TryGetProperty("background", out var background)) UpdateBackground(background);
            if (controller.ActiveProfileId == selected.Id) touchApplicationNames.Clear();
            windowList.Items.Clear();
            var descriptions = new List<string>();
            foreach (var window in message.GetProperty("windows").EnumerateArray())
            {
                uint id = window.GetProperty("id").GetUInt32();
                if (controller.ActiveProfileId == selected.Id && window.TryGetProperty("app_id", out var inputApp) &&
                    inputApp.GetString() is { Length: > 0 } applicationId)
                    touchApplicationNames[applicationId] = window.TryGetProperty("title", out var appTitle)
                        ? $"{appTitle.GetString()} · {applicationId}" : applicationId;
                string title = window.TryGetProperty("title", out var titleValue) ? titleValue.GetString() ?? $"窗口 {id}" : $"窗口 {id}";
                string description = $"{title} · {window.GetProperty("codec")} · {(window.GetProperty("hardware_decode").GetBoolean() ? "GPU 解码" : "PNG/非视频")} · {window.GetProperty("presentation")}";
                descriptions.Add(description);
                var focus = Button("切换");
                focus.Click += async (_, _) => await GuardAsync(() => controller.ActivateWindowAsync(id));
                windowList.Items.Add(Row(description, $"{window.GetProperty("width")} × {window.GetProperty("height")} 逻辑像素", focus));
            }
            if (controller.ActiveProfileId == selected.Id) RefreshTouchApplications();
            actual.Text = descriptions.Count > 0 ? string.Join('\n', descriptions) : "已连接，等待远端应用窗口";
        }
        else if (type == "transfer")
        {
            if (message.TryGetProperty("directory", out var directory)) receiveDirectory = directory.GetString() ?? receiveDirectory;
            string text = message.TryGetProperty("name", out var n) ? n.GetString() + " · " : "";
            string? activity = message.GetProperty("event").GetString();
            text += activity switch
            {
                "upload_started" => "正在发送…",
                "tree_upload_ready" when message.TryGetProperty("stage", out var stage) && stage.GetString() == "commit" => "发送完成，已放入远端剪贴板",
                "tree_upload_ready" => "服务端已接收目录信息",
                "upload_ready" => "文件发送完成",
                "files_begin" or "tree_begin" => "正在接收…",
                "files_done" or "tree_done" => "数据接收完成",
                "download_complete" => "只读输入已就绪，可复制到自己选择的文件夹",
                "files_error" or "tree_error" or "tree_upload_error" or "upload_rejected" or "download_error" => "传输失败",
                _ => "文件传输状态已更新"
            };
            if (message.TryGetProperty("message", out var detail) && detail.GetString() is { Length: > 0 } reason) text += " · " + reason;
            if (message.TryGetProperty("bytes", out var b)) text += $" · {b} bytes";
            transfers.Items.Insert(0, Text(text));
            while (transfers.Items.Count > 100) transfers.Items.RemoveAt(transfers.Items.Count - 1);
        }
    }
    private Task<bool> ConfirmHostAsync(TrustRequest request)
    {
        var result = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        UI(async () =>
        {
            try { result.TrySetResult(await ConfirmAsync("首次连接此 SSH 主机", $"{request.Host}:{request.Port}\n请核对服务器主机指纹：\n\n" + string.Join('\n', request.Fingerprints), "信任并连接")); }
            catch (Exception error) { result.TrySetException(error); }
        });
        return result.Task;
    }
    private async Task<bool> ConfirmAsync(string title, string description, string primary)
    {
        if (activeDialog is not null) throw new InvalidOperationException("请先关闭当前对话框");
        activeDialog = new ContentDialog
        {
            XamlRoot = root.XamlRoot, Title = title, Content = Text(description), PrimaryButtonText = primary,
            CloseButtonText = "取消", DefaultButton = ContentDialogButton.Close
        };
        try { return await activeDialog.ShowAsync() == ContentDialogResult.Primary; }
        finally { activeDialog = null; }
    }
    private async void TrayAction(string action)
    {
        await GuardAsync(async () =>
        {
            if (action is "show" or "diagnostics") { TrayIcon.ShowWindow(hwnd, 9); Activate(); }
            if (action == "diagnostics") navigation.SelectedItem = navigation.MenuItems[5];
            if (action == "connect") await ConnectOrDisconnectAsync();
            if (action == "mute") { audio.IsOn = !audio.IsOn; if (controller.Connected) await ApplyAsync(); }
            if (action == "quit")
            {
                quitting = true; activeDialog?.Hide(); await desktopNotifications.DisposeAsync(); await controller.DisposeAsync(); tray.Dispose();
                Close(); Application.Current.Exit();
            }
        });
    }
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
}
