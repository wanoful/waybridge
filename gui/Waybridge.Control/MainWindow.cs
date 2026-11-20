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
    private readonly TextBlock status = new() { Text = Tr("Disconnected"), TextWrapping = TextWrapping.Wrap };
    private readonly InfoBar notice = new() { IsOpen = false, IsClosable = true };
    private bool notificationServiceWarning;
    private readonly ProfileStore store;
    private readonly CredentialStore secrets = new();
    private readonly SessionController controller;
    private readonly DesktopNotifications desktopNotifications;
    private readonly TrayIcon tray;
    private readonly ObservableCollection<ConnectionProfile> profiles = [];
    private readonly ComboBox profilePicker = L("Select workstation").Bind(new ComboBox { DisplayMemberPath = "Name", MinWidth = 156, MaxWidth = 320, HorizontalAlignment = HorizontalAlignment.Stretch }, ComboBox.PlaceholderTextProperty);
    private readonly TextBox name = new(), host = new(), session = new(), udpHost = new();
    private readonly TextBox fingerprint = new() { IsReadOnly = true };
    private readonly Button revoke = Button(L("Unpair this device"));
    private readonly ComboBox sessionMode = Combo([L("Reuse a session or create one"), L("Attach to an existing session only"), L("Create a new session only")]);
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
    private readonly ComboBox touchApplication = L("Connect to select an app").Bind(new ComboBox { DisplayMemberPath = "Name", MinWidth = 220 }, ComboBox.PlaceholderTextProperty);
    private readonly ComboBox touchAppMode = Combo([L("Automatic"), L("Prefer native touch"), L("Mouse compatibility (press and hold, drag to select)")]);
    private Dictionary<string, string> touchModes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> touchApplicationNames = new(StringComparer.Ordinal);
    private bool loadingTouchApplication;
    private string? editingTouchApplication;
    private readonly ComboBox codec = Combo([L("Auto"), "H.264", "HEVC", "AV1", L("Lossless PNG")]);
    private readonly ComboBox scale = Combo([L("Follow Windows"), "100%", "125%", "150%", "175%", "200%", "250%", "300%", "400%"]);
    private readonly ComboBox clipboard = Combo([L("Two-way sync"), L("Windows → Linux only"), L("Linux → Windows only"), L("Off")]);
    private readonly ComboBox gpu = Combo([L("CPU compatibility path"), L("CUDA color input")]);
    private readonly ComboBox output = new() { MinWidth = 220 };
    private readonly NumberBox fps = Number(60, 1, 120), bitrate = Number(30, 1, 200);
    private readonly Slider volume = new() { Minimum = 0, Maximum = 100, Value = 100, StepFrequency = 1, MinWidth = 200 };
    private readonly Button connect = Button(L("Connect"), true), end = Button(L("End remote session"));
    private readonly Button addProfile = Button(L("Add device")), deleteProfile = Button(L("Remove this device"));
    private string badgePng = "";
    private readonly Image badgePreview = new() { Width = 32, Height = 32 };
    private readonly TextBlock actual = new() { Text = Tr("Encoder / decoder / renderer: Waiting for connection"), TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock appliedVideo = Muted("");
    private readonly TextBlock diagnosticMediaPath = Text(Tr("Waiting for connection"));
    private readonly TextBox applicationSearch = L("Search apps and windows · Ctrl+K").Bind(new TextBox(), TextBox.PlaceholderTextProperty);
    private readonly ItemsRepeater applicationList = new();
    private readonly ListView windowList = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 280 };
    private readonly ToggleSwitch remoteTray = Toggle(true);
    private readonly TextBlock trayStatus = new() { Text = Tr("Tray: Waiting for connection"), TextWrapping = TextWrapping.Wrap };
    private JsonElement? remoteTrayStatus;
    private readonly ListView backgroundList = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 280 };
    private readonly ListView transfers = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 350 };
    private readonly TextBox logs = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, MinHeight = 180, MaxHeight = 320 };
    private readonly Dictionary<string, TextBlock> metrics = new();
    private IReadOnlyList<RemoteApplication> applications = [];
    private ConnectionProfile selected = NewProfile();
    private ContentDialog? activeDialog;
    private bool loading, initialized, quitting, busy, connecting;
    private Task? initialization;
    private ulong lastBytes, lastUpdates, lastTick;
    private string receiveDirectory = "";
    private readonly IntPtr hwnd;
    private readonly List<(string Id, string Name)> audioDevices = [("", Tr("Use the system audio output"))];

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
        string directory = stateDirectory ?? LauncherPreferences.DefaultDirectory;
        store = new(directory);
        controller = new(Path.Combine(AppContext.BaseDirectory, "waybridge-client.exe"));
        desktopNotifications = new(request => controller.SendEngineAsync(request), message => UI(() => Inform(message)));
        controller.Status += _ => UI(() => status.Text = controller.CurrentStatus);
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
        tray = new(action => UI(() => TrayAction(action)), QueueLaunchLink);
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
        AddPage("apps", L("Remote apps"), "\uE71D", ApplicationsPage(), scroll: false);
        AddPage("running", L("Running"), "\uE768", RunningPage());
        AddPage("diagnostics", L("Diagnostics"), "\uE9D9", DiagnosticsPage());
        navigation.MenuItems.Add(L("Settings").Bind(new NavigationViewItemHeader(), ContentControl.ContentProperty));
        AddPage("display", L("Video"), "\uE7F4", DisplayPage());
        AddPage("audio", L("Sound"), "\uE767", AudioPage());
        AddPage("input", L("Input devices"), "\uE765", InputPage());
        AddPage("sharing", L("Sharing and permissions"), "\uE72E", SharingPage());
        AddPage("network", L("Network"), "\uE968", NetworkPage());
        AddPage("connection", L("Connect"), "\uE77B", ConnectionPage());
        AddPage("desktop", L("Desktop integration"), "\uE737", DesktopPage());
        AddPage("general", L("General"), "\uE713", GeneralPage());
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
        root.Loaded += async (_, _) => await GuardAsync(() => initialization ??= InitializeAsync(refreshCatalog: true));
    }
    private void UI(Action action) => DispatcherQueue.TryEnqueue(() => { try { action(); } catch (Exception error) { Error(error); } });
    private void Error(Exception error) => ShowNotice(error.Message, InfoBarSeverity.Error, Tr("Could not complete the operation"));
    private void Inform(string message) => ShowNotice(message, InfoBarSeverity.Informational);
    private void ShowNotice(string message, InfoBarSeverity severity, string title = "", bool notificationWarning = false)
    {
        notificationServiceWarning = notificationWarning;
        notice.Severity = severity;
        notice.Title = title;
        notice.Message = message;
        notice.IsOpen = true;
    }
    private async Task GuardAsync(Func<Task> action)
    {
        try { await action(); }
        catch (OperationCanceledException) { status.Text = Tr("Canceled"); }
        catch (Exception error) { Error(error); }
    }
    private static TextBlock Text(UiText value, double size = 14) => value.Bind(
        new TextBlock { FontSize = size, TextWrapping = TextWrapping.Wrap }, TextBlock.TextProperty);

    private static readonly Lazy<DataTemplate> ChoiceLabelTemplate = new(() => (DataTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load("""
        <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
            <TextBlock Text="{Binding Text, Mode=OneWay}" />
        </DataTemplate>
        """));

    private static ComboBox Combo(UiText[] items)
    {
        var result = new ComboBox { MinWidth = 200, HorizontalAlignment = HorizontalAlignment.Stretch };
        foreach (var item in items)
        {
            var choice = new ComboBoxItem { Content = item.Source };
            if (item.Translate)
            {
                // The closed ComboBox copies the selected item's data and template.
                var label = item.CreateLabel();
                choice.Content = label;
                choice.ContentTemplate = ChoiceLabelTemplate.Value;
                label.Bind(choice, Microsoft.UI.Xaml.Automation.AutomationProperties.NameProperty);
            }
            result.Items.Add(choice);
        }
        result.SelectedIndex = 0; return result;
    }
    private static NumberBox Number(double value, double min, double max) => new()
    { Value = value, Minimum = min, Maximum = max, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact, MinWidth = 150 };
    private static ToggleSwitch Toggle(bool value)
    {
        var control = new ToggleSwitch { IsOn = value };
        L("On").Bind(control, ToggleSwitch.OnContentProperty);
        L("Off").Bind(control, ToggleSwitch.OffContentProperty);
        return control;
    }
    private static ConnectionProfile NewProfile() => new();
    private static Button Button(UiText label, bool primary = false)
    {
        var result = label.Bind(new Button { MinHeight = 36 }, ContentControl.ContentProperty);
        label.Name(result);
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
    private static UIElement Row(UiText title, UiText description, FrameworkElement control)
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
        if (description.Source.Length > 0) labels.Children.Add(Muted(description));
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
    private static UIElement Field(UiText label, Microsoft.UI.Xaml.Controls.Control field)
    {
        field.HorizontalAlignment = HorizontalAlignment.Stretch; field.MaxWidth = double.PositiveInfinity;
        label.Name(field);
        return new StackPanel { Spacing = 5, Children = { Text(label), field } };
    }
    private void AddPage(string id, UiText title, string glyph, UIElement page, bool scroll = true)
    {
        pages[id] = scroll ? new ScrollViewer { Content = page, Padding = new Thickness(26, 20, 26, 24), HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled } : page;
        var item = title.Bind(new NavigationViewItem { Tag = id, Icon = Glyph(glyph) }, ContentControl.ContentProperty);
        title.Name(item);
        title.Bind(item, ToolTipService.ToolTipProperty);
        navigation.MenuItems.Add(item);
    }
    private UIElement BadgeEditor()
    {
        var choose = Button(L("Choose image"));
        var reset = Button(L("Use Waybridge icon"));
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
        return Column(Row(L("Remote app icon badge"), L("Used on Windows window and tray icons. Saved per connection and applied immediately."), badgePreview), Actions(choose, reset));
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
        L("Native multitouch").Name(touch);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchStatus, "NativeTouchStatus");
        touch.Toggled += (_, _) => { if (!loading) UpdateTouchStatus(); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchApplication, "TouchApplication");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(touchAppMode, "TouchApplicationMode");
        touchApplication.SelectionChanged += (_, _) => LoadTouchApplicationMode();
        foreach (var (control, id, label) in new[] { (pen, "NativePenToggle", L("Pressure-sensitive pen")),
                     (gamepads, "GamepadToggle", L("Game controllers")), (gamepadRumble, "GamepadRumbleToggle", L("Controller vibration")) })
        {
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(control, id);
            label.Name(control);
            control.Toggled += (_, _) => { if (!loading) UpdateInputDeviceStatus(); };
        }
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(penPressureGamma, "PenPressureGamma");
        L("Pen pressure curve").Name(penPressureGamma);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(gamepadDeadzone, "GamepadDeadzone");
        L("Stick dead zone percentage").Name(gamepadDeadzone);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(penStatus, "NativePenStatus");
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(gamepadStatus, "GamepadStatus");
        penPressureGamma.SmallChange = .25;
        penPressureGamma.ValueChanged += (_, _) => { if (!loading) UpdateInputDeviceStatus(); };
        gamepadDeadzone.ValueChanged += (_, _) => { if (!loading) UpdateInputDeviceStatus(); };
        var advanced = new Expander
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            Content = Column(
                Row(L("Controller vibration"), L("Allow games to send vibration feedback to a connected controller."), gamepadRumble),
                Row(L("Stick dead zone (%)"), L("Ignore small movements near the center of the stick."), gamepadDeadzone),
                Row(L("Pen pressure curve"), L("1 is linear. A value below 1 makes light pressure more pronounced."), penPressureGamma),
                Row(L("Per-app touch mode"), L("Select a running app. The default adapts to what the app supports."), touchApplication),
                Row(L("Input mode for the selected app"), L("Mouse compatibility supports Windows press-and-hold right-click and drag selection."), touchAppMode))
        };
        L("Vibration, pressure curve, and app compatibility").Bind(advanced, Expander.HeaderProperty);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetAutomationId(advanced, "InputSettingsAdvanced");
        return SettingsPage(L("Input devices"),
            Row(L("Native multitouch (experimental)"), L("Applies immediately. Apps that support touch receive native input. Other apps receive mouse input."), touch),
            touchStatus,
            Row(L("Pressure-sensitive pen (experimental)"), L("Applies immediately. Forwards pressure, tilt, and eraser input."), pen),
            penStatus,
            Row(L("Game controllers (experimental)"), L("Applies immediately. Forwards Windows controllers to this Linux session."), gamepads),
            gamepadStatus,
            Row(L("Local Windows IME"), L("Use the local candidate window and remote preedit."), ime),
            advanced,
            SettingsActions(includeApply: true, automation: true),
            Row(L("Release captured mouse"), L("Use when a game has captured the mouse"), Text("Ctrl + Alt + Shift + F12")));
    }
    private UIElement TransfersPage()
    {
        var send = Button(L("Send files"), true);
        var folder = Button(L("Send folder"));
        var receive = Button(L("Open read-only downloads folder"));
        var close = IconButton(L("Close transfers panel"), "\uE711");
        close.Click += (_, _) => transferDrawer.IsPaneOpen = false;
        var title = new Grid { ColumnSpacing = 8, Children = { Heading(L("File transfers")) } };
        title.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        title.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        Grid.SetColumn(close, 1); title.Children.Add(close);
        send.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (!controller.Connected) throw new InvalidOperationException(Tr("Connect to a workstation first"));
            var picker = new FileOpenPicker(); picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickMultipleFilesAsync();
            if (picked.Count > 0) await controller.SendEngineAsync(new { type = "send_files", paths = picked.Select(p => p.Path).ToArray() });
        });
        folder.Click += async (_, _) => await GuardAsync(async () =>
        {
            if (!controller.Connected) throw new InvalidOperationException(Tr("Connect to a workstation first"));
            var picker = new FolderPicker(); picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickSingleFolderAsync();
            if (picked is not null) await controller.SendEngineAsync(new { type = "send_files", paths = new[] { picked.Path } });
        });
        receive.Click += async (_, _) => await GuardAsync(() =>
        {
            string path = receiveDirectory;
            if (!Directory.Exists(path)) { Inform(Tr("No files have been received, or the downloads folder was removed.")); return Task.CompletedTask; }
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            return Task.CompletedTask;
        });
        return Column(title, Actions(send, folder, receive),
            Muted(L("You can also drag files between Windows and remote windows. Copy read-only downloads to your own folder before editing.")),
            Heading(L("Transfer history"), 17), transfers);
    }
    private UIElement DiagnosticsPage()
    {
        var content = new StackPanel { Spacing = 10, Children = { Heading(L("Diagnostics")), Card(Column(Heading(L("Current video path"), 17), diagnosticMediaPath), new Thickness(15)) } };
        foreach (var (key, label) in new[] { ("rtt", L("Network round-trip time")), ("rate", L("TCP receive rate")), ("updates", L("Window presentation updates (total)")),
            ("decode_queue",L("Decoder queue time")), ("decode",L("Video decode time")), ("ui_queue",L("UI queue time")), ("present",L("Presentation submission")) })
        { var value = Text(Tr("Waiting for capture")); metrics[key] = value; content.Children.Add(Row(label, "", value)); }
        content.Children.Add(Text(L("Round-trip time, queues, decoding, and presentation are measured individually. They do not include total input-to-display latency."), 12));
        var export = Button(L("Export diagnostics")); export.Click += async (_, _) => await GuardAsync(async () =>
        {
            var picker = new FileSavePicker { SuggestedFileName = "waybridge-diagnostics" };
            picker.FileTypeChoices.Add(Tr("Text log"), [".txt"]); WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            var picked = await picker.PickSaveFileAsync();
            if (picked is not null)
            {
                string text = $"Waybridge Control\nGUI architecture: {RuntimeInformation.ProcessArchitecture}\n{actual.Text}\n" +
                    string.Join('\n', metrics.Select(pair => pair.Key + ": " + pair.Value.Text)) + "\n" + string.Join('\n', controller.Logs);
                await File.WriteAllTextAsync(picked.Path, text); Inform(Tr("Diagnostics exported. Session tokens and clipboard contents were excluded."));
            }
        });
        content.Children.Add(export); content.Children.Add(logs);
        return content;
    }
    private async Task InitializeAsync(bool refreshCatalog)
    {
        if (initialized) return;
        var saved = await store.LoadAsync();
        try { launcherPreferences = await LauncherPreferences.LoadAsync(store.DirectoryPath); }
        catch (Exception error) when (error is IOException or JsonException) { Inform(Tr("Launcher preferences are unavailable: ") + error.Message); }
        try
        {
            var found = await Windows.Devices.Enumeration.DeviceInformation.FindAllAsync(Windows.Media.Devices.MediaDevice.GetAudioRenderSelector());
            foreach (var device in found) audioDevices.Add((device.Id, device.Name));
        }
        catch (Exception error) { Inform(Tr("Audio device list is unavailable: ") + error.Message); }
        loading = true;
        theme.SelectedIndex = launcherPreferences.Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        language.SelectedIndex = launcherPreferences.Language switch { "en" => 1, "zh-Hans" => 2, _ => 0 };
        closeToTray.IsOn = launcherPreferences.CloseToTray;
        ApplyLauncherTheme();
        output.ItemsSource = audioDevices.Select(d => d.Name).ToList();
        foreach (var profile in saved) profiles.Add(profile);
        var initial = profiles.FirstOrDefault(profile => profile.Id == launcherPreferences.SelectedProfile) ?? profiles.FirstOrDefault();
        profilePicker.SelectedItem = initial;
        initialized = true;
        Load(initial ?? NewProfile());
        SetDirectoryLayout();
        if (refreshCatalog && initial is not null) await RefreshApplicationsAsync();
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
        string connectLabel = live ? Tr("Disconnect") : controller.IsRunning ? Tr("Cancel connection") : paired ? Tr("Connect") : Tr("Pair");
        connect.Content = connectLabel;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(connect, connectLabel);
        foreach (var button in saveButtons) button.IsEnabled = initialized && hasProfile && !busy;
        foreach (var button in applyButtons) button.IsEnabled = !busy && live;
        foreach (var button in reconnectButtons) button.IsEnabled = paired && !busy && live;
        foreach (var scope in profileScopes) scope.Text = hasProfile ? Tr("Connection · {0}", selected.Name) : Tr("Add a workstation to save connection settings.");
        pairedStatus.Text = paired ? Tr("This Windows device is paired over TLS.") : Tr("This device needs to be paired.");
        UpdateLauncherActions();
        UpdateMicrophoneStatus();
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
            status.Text = Tr("Disconnected");
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
        microphone.IsOn = settings.Microphone;
        output.SelectedIndex = Math.Max(0, audioDevices.FindIndex(d => d.Id == settings.AudioDevice));
        udp.IsOn = settings.Udp; udpHost.Text = settings.UdpHost; hdr.IsOn = settings.Hdr;
        gpu.SelectedIndex = settings.GpuInput == "cuda" ? 1 : 0;
        clipboard.SelectedIndex = Array.IndexOf(new[] { "both", "send", "receive", "off" }, settings.Clipboard);
        remoteTray.IsOn = settings.Tray;
        images.IsOn = settings.ClipboardImages; files.IsOn = settings.Files; dragText.IsOn = settings.DragText; readOnly.IsOn = settings.ReadOnly; ime.IsOn = settings.Ime; touch.IsOn = settings.Touch;
        fileRate.Value = settings.FileRateKib;
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
            Microphone = microphone.IsOn,
            Hdr = hdr.IsOn, Udp = udp.IsOn, UdpHost = udpHost.Text.Trim(), GpuInput = gpu.SelectedIndex == 1 ? "cuda" : "cpu",
            Clipboard = new[] { "both", "send", "receive", "off" }[Math.Max(0, clipboard.SelectedIndex)],
            BadgePng = badgePng, ClipboardImages = images.IsOn, Files = files.IsOn, DragText = dragText.IsOn, ReadOnly = readOnly.IsOn, Ime = ime.IsOn, Touch = touch.IsOn,
            FileRateKib = checked((int)fileRate.Value),
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
        Inform(Tr("Connection settings saved."));
    });
    private async Task StoreProfileAsync(ConnectionProfile profile)
    {
        int index = profiles.ToList().FindIndex(saved => saved.Id == profile.Id);
        if (index < 0) throw new InvalidOperationException(Tr("This connection profile no longer exists. Select another connection."));
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
        string credential = secrets.Read(profile.Id) ?? throw new InvalidOperationException(Tr("Pair this device again"));
        notice.IsOpen = false;
        liveSettings = SettingsWithLauncherPolicies(profile.Settings);
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
        var requested = SettingsWithLauncherPolicies(profile.Settings);
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
            if (launcherPoliciesPending) _ = GuardAsync(ApplyLauncherPoliciesAsync);
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
            var restore = Button(L("Open / restore"));
            restore.IsEnabled = !inputDeviceReadOnly;
            restore.Click += async (_, _) => await GuardAsync(() => controller.SendEngineAsync(new { type = "tray_action", id, epoch, generation, action = "activate" }));
            var menu = Button(L("App menu"));
            menu.IsEnabled = !inputDeviceReadOnly;
            menu.Click += async (_, _) => await GuardAsync(() => controller.SendEngineAsync(new { type = "tray_action", id, epoch, generation, action = "menu" }));
            string appId = item.TryGetProperty("app_id", out var appKey) ? appKey.GetString() ?? "" : "";
            var caption = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = {
                LauncherMark(MatchWindowApplication(appId), 28), Heading(title, 14) } };
            backgroundList.Items.Add(Card(Column(caption,
                state == "NeedsAttention" ? Muted(L("App needs attention")) : Muted(L("Running in background")), Actions(restore, menu)), new Thickness(12)));
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
        string space = Tr(" ");
        bool saved = selected.Settings.Touch;
        string text = Tr("Setting: {0}.", (saved ? Tr("On") : Tr("Off")));
        if (touchStatusProfile != selected.Id) text += space + Tr("This setting will apply when you connect.");
        else if (touchActive is not { } active) text += space + Tr("Checking touch status for this connection.");
        else
        {
            text += space + Tr("Native touch on this connection: {0}.", (active ? Tr("On") : Tr("Off")));
            if (saved && touchSupported == false) text += space + Tr("The Linux server does not support native touch. Update it and reconnect.");
            else if (saved != active) text += space + (touchLiveSupported == false
                ? Tr("Saved. Reconnect to apply.") : Tr("Saved. Apply it to the current connection to use it now."));
            if (touchReadOnly) text += space + Tr("View-only mode has paused remote input.");
        }
        if (touch.IsOn != saved) text += space + Tr("Touch changes have not been saved.");
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
                    label = Tr("Controller {0}", slot + 1);
                }
                connectedGamepads.Add(TouchFlag(device, "reconnecting") == true ? Tr("{0} (waiting to reconnect)", label) : label);
            }
        }
        if (message.TryGetProperty("settings", out var settings) && settings.ValueKind == JsonValueKind.Object)
            inputDeviceReadOnly = TouchFlag(settings, "read_only") ?? inputDeviceReadOnly;
        inputDeviceReadOnly = TouchFlag(message, "read_only") ?? inputDeviceReadOnly;
        UpdateInputDeviceStatus();
    }
    private static string InputDeviceState(bool saved, bool? active, bool? supported, bool? live, string name)
    {
        string space = Tr(" ");
        string text = Tr("Setting: {0}.", saved ? Tr("On") : Tr("Off"));
        if (active is not { } enabled) return text + space + Tr("Checking device status for this connection.");
        text += space + Tr("{0} on this connection: {1}.", name, (enabled ? Tr("On") : Tr("Off")));
        if (saved && supported == false) text += space + Tr("This connection does not support {0}. Update the server and reconnect.", name);
        else if (saved != enabled) text += space + (live == false ? Tr("Saved. Reconnect to apply.") : Tr("Saved. Apply it to the current connection to use it now."));
        return text;
    }
    private void UpdateInputDeviceStatus()
    {
        string space = Tr(" ");
        bool current = inputDeviceStatusProfile == selected.Id;
        string off = Tr("This setting will apply when you connect.");
        string penText = current ? InputDeviceState(selected.Settings.Pen, penActive, penSupported, penLiveSupported, Tr("Pen"))
            : Tr("Setting: {0}.", (selected.Settings.Pen ? Tr("On") : Tr("Off"))) + space + off;
        string gamepadText = current ? InputDeviceState(selected.Settings.Gamepads, gamepadActive, gamepadSupported, gamepadLiveSupported, Tr("Controller forwarding"))
            : Tr("Setting: {0}.", (selected.Settings.Gamepads ? Tr("On") : Tr("Off"))) + space + off;
        if (current)
        {
            if (penActive == true && penSupported != false)
            {
                if (penHardwareAvailable == false) penText += space + Tr("Windows has not detected a pen.");
                else if (penNativeActive == true) penText += space + Tr("The current app is receiving pen input.");
                else if (penNativeActive == false) penText += space + Tr("Waiting for an app that supports pen input.");
            }
            if (gamepadActive == true)
            {
                if (localGamepadCount == 0) gamepadText += space + Tr("Windows has not detected a compatible controller. Check Bluetooth or USB.");
                else if (localGamepadCount is int count) gamepadText += space + Tr("Windows has detected {0} controllers.", count);
                if (connectedGamepads?.Count > 0)
                    gamepadText += space + Tr("Remote devices: ") + string.Join(Tr(", "), connectedGamepads) + Tr(".");
                if (gamepadSupported == true && localGamepadCount > 0 && !inputDeviceReadOnly)
                {
                    if (gamepadActiveLease == false) gamepadText += space + Tr("Controller input is paused. Focus a remote window to resume.");
                    else if (connectedGamepads?.Count is null or 0) gamepadText += space + Tr("Waiting for Linux to create the devices.");
                }
            }
            if (gamepadActive == true && connectedGamepads?.Count > 0 && selected.Settings.GamepadRumble && rumbleSupported == false)
                gamepadText += space + Tr("This connection does not support vibration feedback.");
            if (penError.Length > 0) penText += space + Tr("Pen unavailable: ") + TrDiagnostic(penError);
            if (gamepadError.Length > 0) gamepadText += space + Tr("Controller unavailable: ") + TrDiagnostic(gamepadError);
            if (inputDeviceReadOnly) { penText += space + Tr("View-only mode has paused remote input."); gamepadText += space + Tr("View-only mode has paused remote input."); }
        }
        if (pen.IsOn != selected.Settings.Pen || penPressureGamma.Value != selected.Settings.PenPressureGamma) penText += space + Tr("Pen changes have not been saved.");
        if (gamepads.IsOn != selected.Settings.Gamepads || gamepadRumble.IsOn != selected.Settings.GamepadRumble ||
            gamepadDeadzone.Value != selected.Settings.GamepadDeadzone) gamepadText += space + Tr("Controller changes have not been saved.");
        penStatus.Text = penText; gamepadStatus.Text = gamepadText;
    }
    private void Receive(JsonElement message)
    {
        if (desktopNotifications.Receive(message)) return;
        string? type = message.GetProperty("type").GetString();
        if (type is "status" or "local_settings_applied") ReceiveTouchStatus(message);
        if (type is "status" or "local_settings_applied") ReceiveMicrophoneStatus(message);
        if (type is "status" or "local_settings_applied" or "gamepad_state") ReceiveInputDeviceStatus(message);
        if (type == "notification_status")
        {
            if (message.TryGetProperty("available", out var available))
            {
                if (available.ValueKind == JsonValueKind.False)
                {
                    string? detail = message.TryGetProperty("message", out var reason) && reason.ValueKind == JsonValueKind.String
                        ? reason.GetString() : null;
                    ShowNotice(TrDiagnostic(detail ?? "Remote notification service unavailable."),
                        InfoBarSeverity.Warning, notificationWarning: true);
                }
                else if (available.ValueKind == JsonValueKind.True && notificationServiceWarning)
                {
                    notificationServiceWarning = false;
                    notice.IsOpen = false;
                }
            }
            return;
        }
        if (type == "connected" && message.TryGetProperty("hardware_codecs", out var supported))
        {
            var capabilities = supported.EnumerateArray().Select(v => v.GetInt32()).ToHashSet();
            foreach (var (index, kind) in new[] { (1, 2), (2, 4), (3, 5) })
                ((ComboBoxItem)codec.Items[index]).IsEnabled = capabilities.Contains(kind);
        }
        else if (type == "tray_status")
        {
            remoteTrayStatus = message;
            UpdateRemoteTrayStatus();
        }
        else if (type is "tray_error" or "tray_warning" or "tray_info") Inform(TrDiagnostic(message.GetProperty("message").GetString() ?? "Could not complete the tray action"));
        else if (type == "tray_background") UpdateBackground(message.GetProperty("items"));
        else if (type == "stream_config")
        {
            if (message.GetProperty("ok").GetBoolean())
            {
                touchReadOnly = TouchFlag(message, "read_only") ?? touchReadOnly;
                inputDeviceReadOnly = TouchFlag(message, "read_only") ?? inputDeviceReadOnly;
                UpdateTouchStatus(); UpdateInputDeviceStatus();
                string reason = message.TryGetProperty("fallback_reason", out var fallback) && fallback.GetString() == "session_video_disabled"
                    ? Tr("\nThis session uses PNG. Start a new session to enable video encoding.") : "";
                appliedVideo.Text = Tr("{0} FPS · {1:g} Mbps per window · {2}", message.GetProperty("fps"), message.GetProperty("bitrate").GetInt32()/1000.0, message.GetProperty("codec")) + reason;
                appliedVideo.Visibility = Visibility.Visible;
            }
            else Inform(Tr("Server rejected the settings: ") + TrDiagnostic(message.GetProperty("message").GetString() ?? "Unknown reason"));
        }
        else if (type is "error" or "settings_warning") Inform(TrDiagnostic(message.GetProperty("message").GetString() ?? "Operation failed"));
        else if (type == "timings")
        {
            var stages = message.GetProperty("stages");
            foreach (var pair in new[] { ("control_rtt","rtt"), ("decode_queue","decode_queue"), ("ui_queue","ui_queue"), ("present","present") })
                if (stages.TryGetProperty(pair.Item1, out var value)) metrics[pair.Item2].Text = Tr("{0:F2} ms average / {1:F2} ms maximum", value.GetProperty("avg_ms").GetDouble(), value.GetProperty("max_ms").GetDouble());
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
                metrics["updates"].Text = Tr("{0:F1} updates/sec", (updates-lastUpdates)*1000.0/(tick-lastTick));
            }
            lastTick = tick; lastBytes = bytes; lastUpdates = updates;
            if (message.TryGetProperty("background", out var background)) UpdateBackground(background);
            if (controller.ActiveProfileId == selected.Id) touchApplicationNames.Clear();
            var windows = new List<RemoteWindow>();
            foreach (var window in message.GetProperty("windows").EnumerateArray())
            {
                uint id = window.GetProperty("id").GetUInt32();
                string applicationId = window.TryGetProperty("app_id", out var inputApp) ? inputApp.GetString() ?? "" : "";
                string title = window.TryGetProperty("title", out var titleValue) ? titleValue.GetString() ?? Tr("Window {0}", id) : Tr("Window {0}", id);
                if (controller.ActiveProfileId == selected.Id && applicationId.Length > 0) touchApplicationNames[applicationId] = $"{title} · {applicationId}";
                string videoCodec = Tr(window.GetProperty("codec").GetString() ?? "Waiting for video");
                string decode = window.GetProperty("hardware_decode").GetBoolean() ? Tr("GPU decoding") : Tr("PNG / non-video");
                string path = $"{videoCodec} · {decode} · {window.GetProperty("presentation")}";
                windows.Add(new(id, title, applicationId, path, $"{window.GetProperty("width")} × {window.GetProperty("height")}"));
            }
            if (controller.ActiveProfileId == selected.Id) RefreshTouchApplications();
            if (!remoteWindows.SequenceEqual(windows))
            {
                remoteWindows = windows.ToArray();
                RenderWindows();
                _ = GuardAsync(ApplyLauncherPoliciesAsync);
            }
            actual.Text = diagnosticMediaPath.Text = windows.Count > 0
                ? string.Join('\n', windows.Select(window => $"{window.Title} · {window.Path}"))
                : Tr("Connected. Waiting for remote windows.");
        }
        else if (type == "transfer")
        {
            if (message.TryGetProperty("directory", out var directory)) receiveDirectory = directory.GetString() ?? receiveDirectory;
            string text = message.TryGetProperty("name", out var n) ? n.GetString() + " · " : "";
            string? activity = message.GetProperty("event").GetString();
            if (activity == "progress") { UpdateTransferProgress(message); return; }
            if (activity is "upload_started" or "tree_upload_ready" or "tree_upload_error" or "tree_begin" or "tree_done")
                return; // Displayed in place by the byte-counted transfer row.
            if ((activity is "download_complete" or "download_error") &&
                message.TryGetProperty("batch", out var batch) && batch.TryGetUInt64(out ulong fileBatch) &&
                activeTransfers.ContainsKey(("download", fileBatch))) return;
            text += activity switch
            {
                "upload_ready" => Tr("Files sent"),
                "files_begin" => Tr("Receiving…"),
                "files_done" => Tr("Transfer received"),
                "download_complete" => Tr("The received files are read-only. Copy them to a folder of your choice."),
                "files_error" or "tree_error" or "tree_upload_error" or "upload_rejected" or "download_error" => Tr("Transfer failed"),
                "file_channel_lost" => Tr("The file channel disconnected. Reconnect to resume file transfers."),
                _ => Tr("Transfer status updated")
            };
            if (message.TryGetProperty("message", out var detail) && detail.GetString() is { Length: > 0 } reason) text += " · " + TrDiagnostic(reason);
            if (message.TryGetProperty("bytes", out var b)) text += " · " + Tr("{0} bytes", b);
            AddTransferItem(Text(text));
        }
    }
    private async Task<bool> ConfirmAsync(string title, string description, string primary)
    {
        if (activeDialog is not null) throw new InvalidOperationException(Tr("Close the current dialog first"));
        activeDialog = new ContentDialog
        {
            XamlRoot = root.XamlRoot, RequestedTheme = root.RequestedTheme,
            Title = title, Content = Text(description), PrimaryButtonText = primary,
            CloseButtonText = Tr("Cancel"), DefaultButton = ContentDialogButton.Close
        };
        try { return await activeDialog.ShowAsync() == ContentDialogResult.Primary; }
        finally { activeDialog = null; }
    }
    private async void TrayAction(string action)
    {
        await GuardAsync(async () =>
        {
            if (action is "show" or "diagnostics") ShowControlCenter();
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
