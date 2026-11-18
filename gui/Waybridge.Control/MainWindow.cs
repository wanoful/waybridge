using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Waybridge.Control.Core;
using Windows.Storage.Pickers;
using Windows.System;

namespace Waybridge.Control;

internal sealed partial class MainWindow : Window
{
    private readonly Grid root = new();
    private readonly NavigationView navigation = new()
    {
        PaneDisplayMode = NavigationViewPaneDisplayMode.LeftCompact, CompactPaneLength = 52, IsPaneOpen = false,
        IsPaneToggleButtonVisible = false, IsBackButtonVisible = NavigationViewBackButtonVisible.Collapsed,
        IsSettingsVisible = false, AlwaysShowHeader = false
    };
    private readonly Dictionary<string, UIElement> pages = new();
    private readonly TextBlock status = new() { Text = "未连接", TextWrapping = TextWrapping.Wrap };
    private readonly InfoBar notice = new() { IsOpen = false, IsClosable = true };
    private readonly ProfileStore store;
    private readonly CredentialStore secrets = new();
    private readonly SessionController controller;
    private readonly DesktopNotifications desktopNotifications;
    private readonly TrayIcon tray;
    private readonly ObservableCollection<ConnectionProfile> profiles = [];
    private readonly ComboBox profilePicker = new() { DisplayMemberPath = "Name", MinWidth = 156, MaxWidth = 320, PlaceholderText = "选择工作站", HorizontalAlignment = HorizontalAlignment.Stretch };
    private readonly TextBox name = new(), host = new(), session = new(), udpHost = new();
    private readonly TextBox fingerprint = new() { IsReadOnly = true };
    private readonly Button revoke = Button("撤销此设备配对");
    private readonly ComboBox sessionMode = Combo(["优先接回，必要时新建", "仅接回已有会话", "仅新建会话"]);
    private readonly NumberBox remotePort = Number(49092, 1, 65535);
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
    private readonly Button connect = Button("连接", true), end = Button("结束远端会话");
    private readonly Button addProfile = Button("添加设备"), deleteProfile = Button("移除此设备");
    private string badgePng = "";
    private readonly Image badgePreview = new() { Width = 32, Height = 32 };
    private readonly TextBlock actual = new() { Text = "实际编码／解码／呈现：等待连接", TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock appliedVideo = Muted("");
    private readonly TextBlock diagnosticMediaPath = Text("等待连接");
    private readonly TextBox applicationSearch = new() { PlaceholderText = "搜索应用和窗口 · Ctrl+K" };
    private readonly ItemsRepeater applicationList = new();
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
    private bool loading, initialized, quitting, busy, connecting;
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
        appWindow.Closing += (_, args) =>
        {
            if (quitting) return;
            args.Cancel = true;
            if (launcherPreferences.CloseToTray) TrayIcon.ShowWindow(hwnd, 0);
            else TrayAction("quit");
        };
        try { SystemBackdrop = new MicaBackdrop(); } catch (COMException) { }
        string directory = stateDirectory ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Waybridge");
        store = new(directory);
        controller = new(Path.Combine(AppContext.BaseDirectory, "waybridge-client.exe"));
        desktopNotifications = new(request => controller.SendEngineAsync(request), message => UI(() => Inform(message)));
        controller.Status += text => UI(() => status.Text = text);
        controller.ConnectionChanged += connected => UI(() =>
        {
            desktopNotifications.ConnectionChanged(connected);
            TouchConnectionChanged(connected);
            if (!connected)
            {
                ResetConnectionView();
                if (!controller.IsRunning) liveSettings = null;
            }
            tray?.Update(connected);
            UpdateMode();
        });
        controller.Log += _ => UI(() => logs.Text = string.Join(Environment.NewLine, controller.Logs));
        controller.Message += message =>
        {
            if (desktopNotifications.Receive(message)) return;
            var profileId = controller.ActiveProfileId;
            string? sessionName = controller.CurrentSession;
            UI(() =>
            {
                if (profileId == selected.Id && profileId == controller.ActiveProfileId && sessionName == controller.CurrentSession)
                    Receive(message);
            });
        };
        tray = new(action => UI(() => TrayAction(action)));
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.RowDefinitions.Add(new() { Height = new GridLength(1, GridUnitType.Star) });
        root.RowDefinitions.Add(new() { Height = GridLength.Auto });
        root.Children.Add(LauncherHeader());
        notice.Margin = new Thickness(12, 6, 12, 0); Grid.SetRow(notice, 1); root.Children.Add(notice);
        transferDrawer.Content = navigation;
        transferDrawer.Pane = new ScrollViewer { Content = TransfersPage(), Padding = new Thickness(16), HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(transferDrawer, 2); root.Children.Add(transferDrawer);
        var footer = new Grid { Padding = new Thickness(18, 7, 18, 9), ColumnSpacing = 12 };
        footer.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        footer.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        footer.Children.Add(status);
        Grid.SetColumn(sessionStatus, 1);
        footer.Children.Add(sessionStatus);
        Grid.SetRow(footer, 3); root.Children.Add(footer);
        AddPage("apps", "远端应用", "\uE71D", ApplicationsPage(), scroll: false);
        AddPage("running", "运行中", "\uE768", RunningPage());
        AddPage("diagnostics", "诊断", "\uE9D9", DiagnosticsPage());
        navigation.MenuItems.Add(new NavigationViewItemHeader { Content = "设置" });
        AddPage("display", "画面", "\uE7F4", DisplayPage());
        AddPage("audio", "声音", "\uE767", AudioPage());
        AddPage("input", "输入设备", "\uE765", InputPage());
        AddPage("sharing", "共享与权限", "\uE72E", SharingPage());
        AddPage("network", "网络", "\uE968", NetworkPage());
        AddPage("connection", "连接", "\uE77B", ConnectionPage());
        AddPage("desktop", "桌面集成", "\uE737", DesktopPage());
        AddPage("general", "常规", "\uE713", GeneralPage());
        navigation.SelectionChanged += (_, args) =>
        {
            if (args.SelectedItem is NavigationViewItem item && item.Tag is string id)
                navigation.Content = pages[id];
        };
        navigation.SelectedItem = navigation.MenuItems[0];
        var searchShortcut = new KeyboardAccelerator { Key = VirtualKey.K, Modifiers = VirtualKeyModifiers.Control };
        searchShortcut.Invoked += (_, args) =>
        {
            DispatcherQueue.TryEnqueue(() => applicationSearch.Focus(FocusState.Keyboard));
            args.Handled = true;
        };
        root.KeyboardAccelerators.Add(searchShortcut);
        root.KeyDown += (_, args) =>
        {
            if (args.Key != VirtualKey.Divide && (int)args.Key != 191) return;
            if (FocusManager.GetFocusedElement(root.XamlRoot) is TextBox or PasswordBox or RichEditBox or NumberBox) return;
            applicationSearch.Focus(FocusState.Keyboard);
            args.Handled = true;
        };
        Content = root;
        UpdateMode();
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
    private static ConnectionProfile NewProfile() => new();
    private static Button Button(string label, bool primary = false)
    {
        var result = new Button { Content = label, MinHeight = 36 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(result, label);
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
        var label = Text(title);
        label.FontWeight = FontWeights.SemiBold;
        var labels = new StackPanel { Spacing = 3 };
        labels.Children.Add(label);
        if (description.Length > 0) labels.Children.Add(Muted(description));
        grid.Children.Add(labels);
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
        return Card(grid);
    }
    private static UIElement Field(string label, Microsoft.UI.Xaml.Controls.Control field)
    {
        field.HorizontalAlignment = HorizontalAlignment.Stretch; field.MaxWidth = double.PositiveInfinity;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(field, label);
        return new StackPanel { Spacing = 5, Children = { Text(label), field } };
    }
    private void AddPage(string id, string title, string glyph, UIElement page, bool scroll = true)
    {
        pages[id] = scroll ? new ScrollViewer { Content = page, Padding = new Thickness(26, 20, 26, 24), HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled } : page;
        var item = new NavigationViewItem { Content = title, Tag = id, Icon = Glyph(glyph) };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(item, title);
        ToolTipService.SetToolTip(item, title);
        navigation.MenuItems.Add(item);
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
        return Column(Row("远端图标角标", "用于 Windows 窗口和托盘的远端应用图标。随连接保存，可实时应用。", badgePreview), Actions(choose, reset));
    }
    private async Task UpdateBadgePreviewAsync()
    {
        string current = badgePng;
        var preview = await IconBranding.PreviewAsync(current);
        if (current == badgePng) badgePreview.Source = preview;
    }
    private UIElement InputPage()
    {
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touch, "NativeTouchToggle");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(touch, "原生多点触控");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchStatus, "NativeTouchStatus");
        touch.Toggled += (_, _) => { if (!loading) UpdateTouchStatus(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchApplication, "TouchApplication");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchAppMode, "TouchApplicationMode");
        touchApplication.SelectionChanged += (_, _) => LoadTouchApplicationMode();
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
        var advanced = new Expander
        {
            Header = "震动、压感曲线和应用兼容",
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Content = Column(
                Row("手柄震动", "允许游戏向当前连接的手柄发送震动反馈。", gamepadRumble),
                Row("摇杆死区（%）", "忽略摇杆中心附近的小幅偏移。", gamepadDeadzone),
                Row("笔压力度曲线", "1 为线性。小于 1 时轻压更明显。", penPressureGamma),
                Row("应用触控模式", "选择运行中的应用。默认按应用支持情况自动切换。", touchApplication),
                Row("所选应用的输入方式", "鼠标兼容支持系统长按右键和拖选。", touchAppMode))
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(advanced, "InputSettingsAdvanced");
        return SettingsPage("输入设备",
            Row("原生多点触控（实验）", "实时应用。支持触控的应用使用原生输入，其余应用使用鼠标兼容。", touch),
            touchStatus,
            Row("压感手写笔（实验）", "实时应用。转发压感、倾斜和橡皮擦。", pen),
            penStatus,
            Row("游戏手柄（实验）", "实时应用。向此 Linux 会话转发 Windows 手柄。", gamepads),
            gamepadStatus,
            Row("Windows 本地 IME", "使用本地候选窗口与远端预编辑。", ime),
            advanced,
            SettingsActions(includeApply: true, automation: true),
            Row("紧急释放鼠标", "游戏捕获鼠标时可使用", Text("Ctrl + Alt + Shift + F12")));
    }
    private UIElement TransfersPage()
    {
        var send = Button("发送文件", true);
        var folder = Button("发送目录");
        var receive = Button("打开只读接收文件夹");
        var close = IconButton("关闭传输面板", "\uE711");
        close.Click += (_, _) => transferDrawer.IsPaneOpen = false;
        var title = new Grid { ColumnSpacing = 8, Children = { Heading("文件传输") } };
        title.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        title.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        Grid.SetColumn(close, 1); title.Children.Add(close);
        send.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (!controller.Connected) throw new InvalidOperationException("请先连接工作站");
            var picker = new FileOpenPicker(); picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickMultipleFilesAsync();
            if (picked.Count > 0) await controller.SendEngineAsync(new { type = "send_files", paths = picked.Select(p => p.Path).ToArray() });
        });
        folder.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (!controller.Connected) throw new InvalidOperationException("请先连接工作站");
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
        return Column(title, Actions(send, folder, receive),
            Muted("也可以直接在 Windows 与远端窗口间拖放文件。编辑前请将收到的只读文件复制到自己的文件夹。"),
            Heading("传输记录", 17), transfers);
    }
    private UIElement DiagnosticsPage()
    {
        var content = new StackPanel { Spacing = 10, Children = { Heading("诊断"), Card(Column(Heading("当前画面路径", 17), diagnosticMediaPath), new Thickness(15)) } };
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
        if (initialized) return;
        var saved = await store.LoadAsync();
        try { launcherPreferences = await LauncherPreferences.LoadAsync(store.DirectoryPath); }
        catch (Exception error) when (error is IOException or JsonException) { Inform("启动器偏好不可用：" + error.Message); }
        try
        {
            var found = await Windows.Devices.Enumeration.DeviceInformation.FindAllAsync(Windows.Media.Devices.MediaDevice.GetAudioRenderSelector());
            foreach (var device in found) audioDevices.Add((device.Id, device.Name));
        }
        catch (Exception error) { Inform("音频设备列表不可用：" + error.Message); }
        loading = true;
        theme.SelectedIndex = launcherPreferences.Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        closeToTray.IsOn = launcherPreferences.CloseToTray;
        ApplyLauncherTheme();
        output.ItemsSource = audioDevices.Select(d => d.Name).ToList();
        foreach (var profile in saved) profiles.Add(profile);
        var initial = profiles.FirstOrDefault(profile => profile.Id == launcherPreferences.SelectedProfile) ?? profiles.FirstOrDefault();
        profilePicker.SelectedItem = initial;
        initialized = true;
        Load(initial ?? NewProfile());
        SetDirectoryLayout();
        if (initial is not null) await RefreshApplicationsAsync();
    }
    private void UpdateMode()
    {
        bool hasProfile = profiles.Count > 0;
        bool editable = initialized && !busy && !controller.IsRunning;
        bool live = controller.Connected && controller.ActiveProfileId == selected.Id;
        addProfile.IsEnabled = editable;
        deleteProfile.IsEnabled = editable && hasProfile;
        profilePicker.IsEnabled = initialized && !busy && hasProfile;
        name.IsEnabled = host.IsEnabled = remotePort.IsEnabled = hasProfile && editable;
        reconnect.IsEnabled = session.IsEnabled = sessionMode.IsEnabled = hasProfile && !busy;
        revoke.IsEnabled = paired && editable;
        end.IsEnabled = !busy && live;
        connect.IsEnabled = initialized && (!busy || (connecting && controller.IsRunning && !live));
        connect.Content = live ? "断开" : controller.IsRunning ? "取消连接" : paired ? "连接" : "配对";
        foreach (var button in saveButtons) button.IsEnabled = initialized && hasProfile && !busy;
        foreach (var button in applyButtons) button.IsEnabled = !busy && live;
        foreach (var button in reconnectButtons) button.IsEnabled = paired && !busy && live;
        foreach (var scope in profileScopes) scope.Text = hasProfile ? $"此连接 · {selected.Name}" : "添加工作站后可保存连接设置。";
        pairedStatus.Text = paired ? "此 Windows 设备已通过 TLS 配对。" : "此设备需要配对。";
        UpdateLauncherActions();
    }
    private void Load(ConnectionProfile profile)
    {
        bool changedProfile = selected.Id != profile.Id;
        if (changedProfile)
        {
            ResetTouchStatus(); ResetInputDeviceStatus(); touchApplicationNames.Clear();
            InvalidateCatalog();
            ResetConnectionView();
            transfers.Items.Clear(); receiveDirectory = "";
            MarkTransferSeen();
            commandDirectory.Text = "";
            status.Text = "未连接";
        }
        loading = true; selected = profile;
        name.Text = profile.Name; host.Text = profile.Host;
        session.Text = profile.Session; remotePort.Value = profile.Port;
        fingerprint.Text = profile.CertificateSha256;
        paired = profile.ClientId.Length > 0 && profile.CertificateSha256.Length == 64 && !string.IsNullOrEmpty(secrets.Read(profile.Id));
        sessionMode.SelectedIndex = Array.IndexOf(new[] { "auto", "attach", "new" }, profile.SessionMode);
        reconnect.IsOn = profile.AutoReconnect;
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
        if (changedProfile) FilterApplications();
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
            Name = name.Text.Trim(), Host = host.Text.Trim(), Session = session.Text.Trim(), Port = checked((int)remotePort.Value),
            SessionMode = new[] { "auto", "attach", "new" }[Math.Max(0, sessionMode.SelectedIndex)],
            AutoReconnect = reconnect.IsOn, Settings = settings
        };
        result.Validate(); return result;
    }
    private Task SaveAsync() => RunOperationAsync(async () =>
    {
        var profile = ReadEditor();
        await StoreProfileAsync(profile);
        Inform("连接设置已保存。");
    });
    private async Task StoreProfileAsync(ConnectionProfile profile)
    {
        int index = profiles.ToList().FindIndex(saved => saved.Id == profile.Id);
        if (index < 0) throw new InvalidOperationException("当前连接配置已不存在，请重新选择连接");
        var previous = profiles[index];
        await store.SaveAsync(profiles.Select(saved => saved.Id == profile.Id ? profile : saved));
        loading = true; profiles[index] = profile; profilePicker.SelectedIndex = index; loading = false;
        selected = profile;
        paired = profile.ClientId.Length > 0 && profile.CertificateSha256.Length == 64 && !string.IsNullOrEmpty(secrets.Read(profile.Id));
        if (previous.Host != profile.Host || previous.Port != profile.Port || previous.ClientId != profile.ClientId)
        {
            InvalidateCatalog();
            FilterApplications();
            _ = GuardAsync(RefreshApplicationsAsync);
        }
        UpdateMode(); UpdateTouchStatus(); UpdateInputDeviceStatus();
    }
    private async Task ConnectOrDisconnectAsync()
    {
        if (busy)
        {
            if (connecting && controller.IsRunning && !controller.Connected)
            {
                connect.IsEnabled = false;
                await controller.DisconnectAsync();
            }
            return;
        }
        if (controller.IsRunning) await RunOperationAsync(controller.DisconnectAsync);
        else if (!paired) await PairDeviceAsync();
        else await RunOperationAsync(() => ConnectProfileAsync(selected), canCancelConnect: true);
    }
    private async Task ConnectProfileAsync(ConnectionProfile profile)
    {
        string credential = secrets.Read(profile.Id) ?? throw new InvalidOperationException("此设备需要重新配对");
        notice.IsOpen = false;
        liveSettings = SettingsWithLauncherTouch(profile.Settings);
        desktopNotifications.SharingChanged(profile.Settings.Notifications);
        var connection = controller.ConnectAsync(profile with { Settings = liveSettings }, credential);
        UpdateMode();
        await connection;
        if (controller.Connected && controller.ActiveProfileId == profile.Id && controller.CurrentSession is { } actualSession &&
            (profile.Session != actualSession || profile.SessionMode == "new"))
        {
            var updated = profile with { Session = actualSession, SessionMode = profile.SessionMode == "new" ? "auto" : profile.SessionMode };
            await StoreProfileAsync(updated);
            session.Text = updated.Session;
            sessionMode.SelectedIndex = updated.SessionMode == "attach" ? 1 : 0;
        }
        _ = GuardAsync(RefreshApplicationsAsync);
    }
    private Task ApplyAsync() => RunOperationAsync(async () =>
    {
        var profile = ReadEditor();
        var requested = SettingsWithLauncherTouch(profile.Settings);
        await controller.ApplyAsync(requested, expectedProfileId: profile.Id);
        liveSettings = requested;
        desktopNotifications.SharingChanged(profile.Settings.Notifications);
        await StoreProfileAsync(profile);
    });
    private async Task RunOperationAsync(Func<Task> operation, bool canCancelConnect = false)
    {
        if (busy || quitting) return;
        busy = true; connecting = canCancelConnect; UpdateMode();
        try { await operation(); }
        finally
        {
            busy = false; connecting = false; UpdateMode();
            if (launcherTouchPending) _ = GuardAsync(ApplyLauncherTouchAsync);
        }
    }
    private void UpdateBackground(JsonElement items)
    {
        string snapshot = items.GetRawText();
        if (snapshot == backgroundSnapshot && backgroundReadOnly == inputDeviceReadOnly) return;
        backgroundSnapshot = snapshot;
        backgroundReadOnly = inputDeviceReadOnly;
        backgroundList.Items.Clear();
        foreach (var item in items.EnumerateArray())
        {
            string id = item.GetProperty("id").GetString() ?? "";
            ulong epoch = item.GetProperty("epoch").GetUInt64(), generation = item.GetProperty("generation").GetUInt64();
            string title = item.GetProperty("title").GetString() ?? "";
            if (title.Length == 0) title = item.GetProperty("app_id").GetString() ?? id;
            string state = item.TryGetProperty("status", out var value) ? value.GetString() ?? "" : "";
            var restore = Button("打开／恢复");
            restore.IsEnabled = !inputDeviceReadOnly;
            restore.Click += async (_, _) => await GuardAsync(() => controller.SendEngineAsync(new { type = "tray_action", id, epoch, generation, action = "activate" }));
            var menu = Button("应用菜单");
            menu.IsEnabled = !inputDeviceReadOnly;
            menu.Click += async (_, _) => await GuardAsync(() => controller.SendEngineAsync(new { type = "tray_action", id, epoch, generation, action = "menu" }));
            string appId = item.TryGetProperty("app_id", out var appKey) ? appKey.GetString() ?? "" : "";
            var caption = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = {
                LauncherMark(MatchWindowApplication(appId), 28), Heading(title, 14) } };
            backgroundList.Items.Add(Card(Column(caption,
                state == "NeedsAttention" ? Muted("应用需要注意") : Muted("后台运行"), Actions(restore, menu)), new Thickness(12)));
        }
        backgroundSection.Visibility = backgroundList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdateDockVisibility();
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
                if (localGamepadCount == 0) gamepadText += "Windows 未检测到可转发的手柄。请检查蓝牙或 USB 连接。";
                else if (localGamepadCount is int count) gamepadText += $"Windows 已识别 {count} 个手柄。";
                if (connectedGamepads?.Count > 0)
                    gamepadText += "远端设备：" + string.Join("、", connectedGamepads) + "。";
                if (gamepadSupported == true && localGamepadCount > 0 && !inputDeviceReadOnly)
                {
                    if (gamepadActiveLease == false) gamepadText += "手柄输入已暂停，请先聚焦远端窗口。";
                    else if (connectedGamepads?.Count is null or 0) gamepadText += "正在等待 Linux 创建设备。";
                }
            }
            if (gamepadActive == true && connectedGamepads?.Count > 0 && selected.Settings.GamepadRumble && rumbleSupported == false)
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
                    ? "\n当前会话使用 PNG。启用视频编码需要新建会话。" : "";
                appliedVideo.Text = $"{message.GetProperty("fps")} FPS · {message.GetProperty("bitrate").GetInt32()/1000.0:g} Mbps/窗口 · {message.GetProperty("codec")}" + reason;
                appliedVideo.Visibility = Visibility.Visible;
            }
            else Inform("服务端未接受设置：" + message.GetProperty("message").GetString());
        }
        else if (type is "error" or "settings_warning") Inform(message.GetProperty("message").GetString() ?? "操作失败");
        else if (type == "timings")
        {
            var stages = message.GetProperty("stages");
            foreach (var pair in new[] { ("control_rtt","rtt"), ("decode_queue","decode_queue"), ("ui_queue","ui_queue"), ("present","present") })
                if (stages.TryGetProperty(pair.Item1, out var value)) metrics[pair.Item2].Text = $"{value.GetProperty("avg_ms").GetDouble():F2} ms 平均 / {value.GetProperty("max_ms").GetDouble():F2} ms 最大";
            if (stages.TryGetProperty("control_rtt", out var rtt)) quickRtt.Text = $"{rtt.GetProperty("avg_ms").GetDouble():0.#} ms";
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
            var windows = new List<RemoteWindow>();
            foreach (var window in message.GetProperty("windows").EnumerateArray())
            {
                uint id = window.GetProperty("id").GetUInt32();
                string applicationId = window.TryGetProperty("app_id", out var inputApp) ? inputApp.GetString() ?? "" : "";
                string title = window.TryGetProperty("title", out var titleValue) ? titleValue.GetString() ?? $"窗口 {id}" : $"窗口 {id}";
                if (controller.ActiveProfileId == selected.Id && applicationId.Length > 0) touchApplicationNames[applicationId] = $"{title} · {applicationId}";
                string path = $"{window.GetProperty("codec")} · {(window.GetProperty("hardware_decode").GetBoolean() ? "GPU 解码" : "PNG/非视频")} · {window.GetProperty("presentation")}";
                windows.Add(new(id, title, applicationId, path, $"{window.GetProperty("width")} × {window.GetProperty("height")}"));
            }
            if (controller.ActiveProfileId == selected.Id) RefreshTouchApplications();
            if (!remoteWindows.SequenceEqual(windows))
            {
                remoteWindows = windows.ToArray();
                RenderWindows();
                _ = GuardAsync(ApplyLauncherTouchAsync);
            }
            actual.Text = diagnosticMediaPath.Text = windows.Count > 0
                ? string.Join('\n', windows.Select(window => $"{window.Title} · {window.Path}"))
                : "已连接，等待远端应用窗口";
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
            MarkTransferActivity();
        }
    }
    private async Task<bool> ConfirmAsync(string title, string description, string primary)
    {
        if (activeDialog is not null) throw new InvalidOperationException("请先关闭当前对话框");
        activeDialog = new ContentDialog
        {
            XamlRoot = root.XamlRoot, RequestedTheme = root.RequestedTheme,
            Title = title, Content = Text(description), PrimaryButtonText = primary,
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
            if (action == "diagnostics") ShowPage("diagnostics");
            if (action == "connect") await ConnectOrDisconnectAsync();
            if (action == "mute") await QuickSettingAsync(audioSetting: true);
            if (action == "quit")
            {
                quitting = true; activeDialog?.Hide(); InvalidateCatalog(); await desktopNotifications.DisposeAsync(); await controller.DisposeAsync(); tray.Dispose();
                Close(); Application.Current.Exit();
            }
        });
    }
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
}
