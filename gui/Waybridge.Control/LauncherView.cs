using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Waybridge.Control.Core;
using Windows.System;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private LauncherPreferences launcherPreferences = new();
    private readonly SemaphoreSlim preferencesSave = new(1);
    private CancellationTokenSource? catalogLoad;
    private readonly TextBlock sessionStatus = Muted("");
    private readonly TextBlock catalogStatus = Muted("");
    private readonly TextBlock applicationsCount = Muted("");
    private readonly VariableSizedWrapGrid pinnedItems = Tiles();
    private readonly VariableSizedWrapGrid recentItems = Tiles();
    private readonly StackPanel pinnedSection = new() { Spacing = 6 };
    private readonly StackPanel favoritesSection = new() { Spacing = 12 };
    private readonly Border directoryHeader = new();
    private readonly TranslateTransform directoryOffset = new();
    private readonly Grid catalogContent = new() { Margin = new Thickness(24, 16, 24, 20) };
    private readonly ScrollViewer catalogScroll = new()
    {
        HorizontalScrollMode = ScrollMode.Disabled, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto, ZoomMode = ZoomMode.Disabled
    };
    private readonly Button matchingWindows = Button("查看匹配窗口");
    private readonly TextBlock runningEmpty = Muted("");
    private readonly Grid dock = new() { ColumnSpacing = 12, RowSpacing = 10 };
    private readonly TextBlock pinHint = Muted("从应用菜单固定常用应用。");
    private readonly TextBlock quickRtt = Muted("—");
    private readonly Ellipse connectionDot = new() { Width = 8, Height = 8, VerticalAlignment = VerticalAlignment.Center };
    private readonly ComboBox theme = Combo(["跟随 Windows", "浅色", "深色"]);
    private readonly ToggleSwitch closeToTray = Toggle(true);
    private readonly Button muteQuick = Button("静音"), readOnlyQuick = IconButton("只读查看", "\uE890");
    private readonly MenuFlyoutItem endSessionQuick = new() { Text = "结束远端会话" };
    private readonly Button modeButton = IconButton("切换为紧凑列表", "\uE8FD");
    private readonly InfoBadge transferBadge = new() { Visibility = Visibility.Collapsed, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, IsHitTestVisible = false };
    private readonly SplitView transferDrawer = new()
    {
        Style = LauncherStyles.Transfers, PanePlacement = SplitViewPanePlacement.Right,
        DisplayMode = SplitViewDisplayMode.Overlay, OpenPaneLength = 390
    };
    private int unreadTransfers;
    private readonly VariableSizedWrapGrid categories = new() { Orientation = Orientation.Horizontal, ItemWidth = 59, ItemHeight = 35 };
    private static readonly (string Name, string[] Tags)[] AppCategories =
    [
        ("全部", []), ("图形", ["Graphics", "AudioVideo", "Video", "Audio"]),
        ("开发", ["Development"]), ("办公", ["Office", "Network", "Education"]),
        ("游戏", ["Game"]), ("系统", ["System", "Settings", "Utility"]), ("其他", [])
    ];
    private int category;
    private readonly StackLayout listLayout = new() { Spacing = 3 };
    private readonly UniformGridLayout gridLayout = new()
    {
        Orientation = Orientation.Horizontal, MinItemWidth = 154, MinItemHeight = AppTileCellHeight,
        ItemsStretch = UniformGridLayoutItemsStretch.Fill
    };
    private IReadOnlyList<RemoteApplication> filteredApplications = [];
    private readonly Button refreshApps = Button("刷新目录");
    private readonly Button emptyAdd = Button("添加工作站", true);
    private readonly StackPanel emptyLauncher = new() { Spacing = 14 };
    private readonly StackPanel windowSection = new() { Spacing = 7 };
    private readonly StackPanel backgroundSection = new() { Spacing = 7 };
    private readonly StackPanel recentSection = new() { Spacing = 7 };
    private RemoteWindow[] remoteWindows = [];
    private ClientSettings? liveSettings;
    private string? backgroundSnapshot;
    private bool backgroundReadOnly;
    private sealed record RemoteWindow(uint Id, string Title, string AppId, string Path, string Size);

    private static FontIcon Glyph(string glyph, double size = 16) => new()
    {
        Glyph = glyph, FontSize = size, FontFamily = new FontFamily("Segoe Fluent Icons")
    };

    private static Button IconButton(string label, string glyph)
    {
        var button = Button(label);
        button.Content = Glyph(glyph);
        button.MinWidth = 36;
        button.Padding = new Thickness(8, 4, 8, 4);
        ToolTipService.SetToolTip(button, label);
        return button;
    }

    private static VariableSizedWrapGrid Tiles() => new()
    { Orientation = Orientation.Horizontal, ItemWidth = 154, ItemHeight = AppTileCellHeight };

    private static TextBlock Muted(string text)
    {
        var label = Text(text, 12);
        label.Style = LauncherStyles.Muted;
        return label;
    }

    private static TextBlock Heading(string text, double size = 24)
    {
        var title = Text(text, size);
        title.FontWeight = FontWeights.SemiBold;
        return title;
    }

    private static Border Card(UIElement content, Thickness padding = default) =>
        new() { Style = LauncherStyles.Card, CornerRadius = new CornerRadius(8), Child = content, Padding = padding };

    private void ShowPage(string id) => navigation.SelectedItem = navigation.MenuItems.OfType<NavigationViewItem>().Single(item => item.Tag as string == id);
    private string? ActivePage => (navigation.SelectedItem as NavigationViewItem)?.Tag as string;

    private UIElement LauncherHeader()
    {
        const double height = 36;
        profilePicker.ItemsSource = profiles;
        profilePicker.MinWidth = 128;
        profilePicker.MaxWidth = 196;
        AutomationProperties.SetName(profilePicker, "选择工作站");
        profilePicker.SelectionChanged += async (_, _) =>
        {
            if (!loading && profilePicker.SelectedItem is ConnectionProfile profile && profile.Id != selected.Id)
                await GuardAsync(() => RunOperationAsync(async () =>
                {
                    if (controller.IsRunning) await controller.DisconnectAsync();
                    applicationSearch.Text = "";
                    await SelectProfileAsync(profile);
                }));
        };
        connect.Click += async (_, _) => await GuardAsync(ConnectOrDisconnectAsync);
        var menu = new MenuFlyout();
        var add = new MenuFlyoutItem { Text = "添加设备", Icon = Glyph("\uE710") };
        add.Click += async (_, _) => await GuardAsync(PairDeviceAsync);
        var configure = new MenuFlyoutItem { Text = "连接设置", Icon = Glyph("\uE713") };
        configure.Click += (_, _) => ShowPage("connection");
        muteQuick.Click += async (_, _) => await GuardAsync(() => QuickSettingAsync(audioSetting: true));
        readOnlyQuick.Click += async (_, _) => await GuardAsync(() => QuickSettingAsync(audioSetting: false));
        endSessionQuick.Click += async (_, _) => await GuardAsync(ConfirmEndAsync);
        foreach (MenuFlyoutItemBase item in new MenuFlyoutItemBase[] { add, configure, new MenuFlyoutSeparator(), endSessionQuick })
            menu.Items.Add(item);
        var more = IconButton("管理工作站", "\uE712");
        more.Flyout = menu;
        menu.Opening += (_, _) => add.IsEnabled = !busy && !controller.IsRunning;
        var hostControls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center,
            Children = { connectionDot, profilePicker, connect, more } };

        var audioButton = IconButton("声音与音量", "\uE767");
        audioButton.Flyout = AudioFlyout();
        var linkButton = IconButton("连接质量", "\uE839");
        linkButton.Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, Children = { Glyph("\uE839"), quickRtt } };
        var linkSummary = Text("");
        var diagnostics = Button("打开诊断");
        var linkFlyout = new Flyout { Content = Column(Heading("连接质量", 16), linkSummary, diagnostics) };
        linkFlyout.Opening += (_, _) => linkSummary.Text = $"往返延迟 · {quickRtt.Text}\n{diagnosticMediaPath.Text}";
        diagnostics.Click += (_, _) => { linkFlyout.Hide(); ShowPage("diagnostics"); };
        linkButton.Flyout = linkFlyout;
        var transfer = IconButton("文件传输", "\uE8B7");
        transfer.Click += (_, _) => { transferDrawer.IsPaneOpen = !transferDrawer.IsPaneOpen; MarkTransferSeen(); };
        var transferAction = new Grid { Children = { transfer, transferBadge } };
        var settings = IconButton("设置", "\uE713");
        settings.Click += (_, _) => ShowPage("display");
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center,
            Children = { readOnlyQuick, audioButton, linkButton, transferAction, settings } };

        AutomationProperties.SetName(applicationSearch, "搜索远端应用、窗口或命令");
        applicationSearch.TextChanged += (_, _) =>
        {
            if (applicationSearch.Text.Trim().Length > 0 && (CommandMode || ActivePage is not ("apps" or "running"))) ShowPage("apps");
            RefreshLauncherSearch();
        };
        applicationSearch.KeyDown += SearchKeyDown;
        foreach (var control in new Microsoft.UI.Xaml.Controls.Control[] { profilePicker, connect, more, applicationSearch, readOnlyQuick, audioButton, linkButton, transfer, settings })
        {
            control.Height = control.MinHeight = height;
            control.VerticalAlignment = VerticalAlignment.Center;
            control.VerticalContentAlignment = VerticalAlignment.Center;
        }
        var header = new Grid { Padding = new Thickness(16, 10, 14, 10), ColumnSpacing = 11 };
        header.RowDefinitions.Add(new() { Height = GridLength.Auto });
        header.RowDefinitions.Add(new() { Height = GridLength.Auto });
        header.RowDefinitions.Add(new() { Height = GridLength.Auto });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        header.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        header.Children.Add(hostControls);
        Grid.SetColumn(applicationSearch, 1); header.Children.Add(applicationSearch);
        Grid.SetColumn(actions, 2); header.Children.Add(actions);
        header.SizeChanged += (_, args) =>
        {
            double width = args.NewSize.Width;
            bool narrow = width < 620;
            bool twoRows = width < 850;
            profilePicker.MinWidth = narrow ? 92 : 128;
            profilePicker.MaxWidth = narrow ? 145 : 196;
            Grid.SetRow(actions, narrow ? 1 : 0);
            Grid.SetRow(applicationSearch, narrow ? 2 : twoRows ? 1 : 0);
            Grid.SetColumn(applicationSearch, twoRows ? 0 : 1);
            Grid.SetColumnSpan(applicationSearch, twoRows ? 3 : 1);
            applicationSearch.Margin = new Thickness(0, twoRows ? 8 : 0, 0, 0);
            actions.Margin = new Thickness(0, narrow ? 8 : 0, 0, 0);
            transferDrawer.OpenPaneLength = Math.Min(400, Math.Max(210, width - 20));
        };
        return header;
    }

    private UIElement ApplicationsPage()
    {
        AutomationProperties.SetName(applicationList, "远端应用目录");
        refreshApps.Click += async (_, _) => await GuardAsync(async () =>
        {
            InvalidateIcons();
            await RefreshApplicationsAsync();
            FilterApplications();
            RenderWindows();
        });
        emptyAdd.Click += async (_, _) => await GuardAsync(PairDeviceAsync);

        var icon = Glyph("\uE7F4", 28);
        icon.HorizontalAlignment = HorizontalAlignment.Left;
        emptyLauncher.Children.Add(icon);
        emptyLauncher.Children.Add(Heading("连接你的 Linux 工作站", 22));
        emptyLauncher.Children.Add(Text("配对后即可搜索并打开工作站上的应用。"));
        emptyLauncher.Children.Add(emptyAdd);
        pinnedSection.Children.Add(Muted("已固定")); pinnedSection.Children.Add(pinnedItems);
        recentSection.Children.Add(Muted("最近启动")); recentSection.Children.Add(recentItems);
        favoritesSection.Children.Add(Heading("固定与最近", 22));
        favoritesSection.Margin = new Thickness(0, 0, 0, 20);
        favoritesSection.Children.Add(pinHint);
        favoritesSection.Children.Add(pinnedSection);
        favoritesSection.Children.Add(recentSection);

        var directoryTitleBar = new Grid { ColumnSpacing = 8 };
        directoryTitleBar.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        directoryTitleBar.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var directoryTitle = new StackPanel { Spacing = 2, Children = { Heading("全部应用", 19), applicationsCount } };
        directoryTitleBar.Children.Add(directoryTitle);
        var directoryActions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { refreshApps, modeButton } };
        Grid.SetColumn(directoryActions, 1); directoryTitleBar.Children.Add(directoryActions);
        modeButton.Click += async (_, _) => await GuardAsync(async () =>
        {
            launcherPreferences.CompactApps = !launcherPreferences.CompactApps;
            SetDirectoryLayout();
            FilterApplications(keepDirectoryAtTop: true);
            await SaveLauncherPreferencesAsync();
        });
        for (int index = 0; index < AppCategories.Length; index++)
        {
            int selectedCategory = index;
            var chip = new ToggleButton { Content = AppCategories[index].Name, IsChecked = index == 0, Padding = new Thickness(8, 3, 8, 3),
                HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0, 0, 6, 4) };
            chip.Click += (_, _) => { category = selectedCategory; UpdateCategories(); FilterApplications(keepDirectoryAtTop: true); };
            categories.Children.Add(chip);
        }
        matchingWindows.Visibility = Visibility.Collapsed;
        matchingWindows.Click += (_, _) => ShowPage("running");
        catalogStatus.Visibility = Visibility.Collapsed;
        directoryHeader.RenderTransform = directoryOffset;
        directoryHeader.Padding = new Thickness(0, 9, 0, 7);
        directoryHeader.Child = new StackPanel { Spacing = 5, Children = { directoryTitleBar, categories, matchingWindows, catalogStatus } };

        applicationList.ItemTemplate = (DataTemplate)XamlReader.Load("""
            <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
                <ContentControl IsTabStop="False" HorizontalContentAlignment="Stretch" VerticalContentAlignment="Stretch" />
            </DataTemplate>
            """);
        applicationList.ElementPrepared += (_, args) =>
        {
            if (args.Index < filteredApplications.Count && args.Element is ContentControl container)
                container.Content = AppTile(filteredApplications[args.Index], launcherPreferences.CompactApps);
        };
        applicationList.ElementClearing += (_, args) => { if (args.Element is ContentControl container) container.Content = null; };
        SetDirectoryLayout();
        for (int i = 0; i < 4; i++) catalogContent.RowDefinitions.Add(new() { Height = GridLength.Auto });
        catalogContent.Children.Add(favoritesSection);
        Grid.SetRow(applicationList, 2); catalogContent.Children.Add(applicationList);
        var commands = BuildCommandPane();
        Grid.SetRow(commands, 3); catalogContent.Children.Add(commands);
        // Draw the sticky strip over the scrolling tiles.
        Grid.SetRow(directoryHeader, 1); catalogContent.Children.Add(directoryHeader);
        catalogScroll.Content = catalogContent;
        catalogScroll.ViewChanged += (_, _) => UpdateDirectoryStickiness();
        favoritesSection.SizeChanged += (_, _) => UpdateDirectoryStickiness();
        var empty = Card(emptyLauncher, new Thickness(22));
        empty.Margin = new Thickness(24, 20, 24, 20);
        empty.VerticalAlignment = VerticalAlignment.Top;
        return new Grid { Children = { catalogScroll, empty } };
    }

    private UIElement RunningPage()
    {
        AutomationProperties.SetName(windowList, "正在运行的窗口");
        AutomationProperties.SetName(backgroundList, "远端后台应用");
        windowSection.Children.Add(Heading("远端窗口", 17)); windowSection.Children.Add(windowList);
        backgroundSection.Children.Add(Heading("后台应用", 17)); backgroundSection.Children.Add(backgroundList);
        dock.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        dock.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        dock.RowDefinitions.Add(new() { Height = GridLength.Auto });
        dock.RowDefinitions.Add(new() { Height = GridLength.Auto });
        dock.Children.Add(windowSection);
        dock.Children.Add(backgroundSection);
        dock.SizeChanged += (_, args) => LayoutDock(args.NewSize.Width);
        windowSection.Visibility = backgroundSection.Visibility = Visibility.Collapsed;
        UpdateDockVisibility();
        return Column(Heading("运行中", 22), runningEmpty, dock);
    }

    private bool Matches(params string[] values)
    {
        string query = applicationSearch.Text.Trim();
        return values.Any(value => value.Contains(query, StringComparison.CurrentCultureIgnoreCase));
    }

    private bool MatchesApp(RemoteApplication app) => Matches(app.Name, app.Id, app.GenericName, app.Comment)
        || app.Keywords.Any(keyword => Matches(keyword));

    private bool MatchesCategory(RemoteApplication app) => category switch
    {
        0 => true,
        6 => !AppCategories.Skip(1).Take(5).Any(group => group.Tags.Intersect(app.Categories, StringComparer.OrdinalIgnoreCase).Any()),
        _ => AppCategories[category].Tags.Intersect(app.Categories, StringComparer.OrdinalIgnoreCase).Any()
    };

    private void UpdateCategories()
    {
        for (int index = 0; index < categories.Children.Count; index++)
            ((ToggleButton)categories.Children[index]).IsChecked = index == category;
    }

    private void SetDirectoryLayout()
    {
        bool compact = launcherPreferences.CompactApps;
        applicationList.Layout = compact ? listLayout : gridLayout;
        modeButton.Content = Glyph(compact ? "\uE80A" : "\uE8FD");
        string label = compact ? "切换为网格" : "切换为紧凑列表";
        AutomationProperties.SetName(modeButton, label);
        ToolTipService.SetToolTip(modeButton, label);
    }

    private double DirectoryTop => catalogContent.RowDefinitions[0].ActualHeight + catalogContent.Margin.Top;

    private void UpdateDirectoryStickiness()
    {
        if (catalogContent.RowDefinitions.Count == 0) return;
        double offset = directoryHeader.Visibility == Visibility.Visible ? Math.Max(0, catalogScroll.VerticalOffset - DirectoryTop) : 0;
        directoryOffset.Y = offset;
        var style = offset > 0 ? LauncherStyles.Directory : null;
        if (directoryHeader.Style != style) directoryHeader.Style = style;
    }

    private void LayoutDock(double width)
    {
        bool both = windowList.Items.Count > 0 && backgroundList.Items.Count > 0;
        bool sideBySide = both && width >= 590;
        dock.ColumnDefinitions[1].Width = sideBySide ? new GridLength(1, GridUnitType.Star) : GridLength.Auto;
        Grid.SetColumn(backgroundSection, sideBySide ? 1 : 0);
        Grid.SetRow(backgroundSection, both && !sideBySide ? 1 : 0);
    }

    private void UpdateDockVisibility()
    {
        LayoutDock(dock.ActualWidth);
        bool hasRunning = windowList.Items.Count > 0 || backgroundList.Items.Count > 0;
        dock.Visibility = hasRunning ? Visibility.Visible : Visibility.Collapsed;
        runningEmpty.Visibility = hasRunning ? Visibility.Collapsed : Visibility.Visible;
        runningEmpty.Text = controller.Connected ? "当前没有匹配的远端窗口或后台应用。" : "连接工作站后可查看远端窗口和后台应用。";
    }

    private void FilterApplications(bool keepDirectoryAtTop = false)
    {
        bool scrollToDirectory = keepDirectoryAtTop && catalogScroll.VerticalOffset >= DirectoryTop;
        filteredApplications = applications.Where(app => MatchesApp(app) && MatchesCategory(app)).ToArray();
        applicationList.ItemsSource = filteredApplications;
        applicationList.Visibility = filteredApplications.Count > 0 && !CommandMode ? Visibility.Visible : Visibility.Collapsed;
        applicationsCount.Text = applications.Count == 0 ? "" : filteredApplications.Count == 0 ? "没有匹配的应用" : $"{filteredApplications.Count} 个应用";
        applicationsCount.Visibility = applicationsCount.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        RenderRecent();
        if (scrollToDirectory)
            DispatcherQueue.TryEnqueue(() => catalogScroll.ChangeView(null, Math.Min(DirectoryTop, catalogScroll.ScrollableHeight), null, true));
    }

    private void RenderRecent()
    {
        pinnedItems.Children.Clear();
        recentItems.Children.Clear();
        var pinned = launcherPreferences.Pinned.GetValueOrDefault(selected.Id) ?? [];
        var recent = launcherPreferences.Recent.GetValueOrDefault(selected.Id) ?? [];
        var pins = pinned.Select(LatestApp).Where(MatchesApp).ToList();
        foreach (var app in pins) pinnedItems.Children.Add(AppTile(app, false));
        foreach (var app in recent.Select(LatestApp).Where(app => MatchesApp(app) && !pinned.Any(pin => pin.Id == app.Id)).Take(6))
            recentItems.Children.Add(AppTile(app, false));
        pinnedSection.Visibility = pins.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        recentSection.Visibility = recentItems.Children.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        pinHint.Visibility = pins.Count == 0 && recentItems.Children.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        favoritesSection.Visibility = applicationSearch.Text.Trim().Length == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private RemoteApplication LatestApp(RemoteApplication app) => applications.FirstOrDefault(candidate => candidate.Id == app.Id) ?? app;

    private void RenderWindows()
    {
        windowList.Items.Clear();
        foreach (var window in remoteWindows.Where(window => Matches(window.Title, window.AppId)))
        {
            var focus = Button(window.Title);
            focus.HorizontalAlignment = HorizontalAlignment.Stretch;
            focus.HorizontalContentAlignment = HorizontalAlignment.Left;
            var name = Heading(window.Title, 14);
            name.MaxLines = 1; name.TextTrimming = TextTrimming.CharacterEllipsis;
            focus.Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9, Children = {
                LauncherMark(MatchWindowApplication(window.AppId), 29), Column(name, Muted(window.Path)) } };
            AutomationProperties.SetName(focus, $"切回窗口：{window.Title}");
            focus.Click += async (_, _) => await GuardAsync(() => FocusRemoteWindowAsync(window));
            windowList.Items.Add(focus);
        }
        windowSection.Visibility = windowList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        matchingWindows.Visibility = !CommandMode && applicationSearch.Text.Trim().Length > 0 && windowList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        matchingWindows.Content = $"查看 {windowList.Items.Count} 个匹配窗口";
        UpdateDockVisibility();
    }

    private Task FocusRemoteWindowAsync(RemoteWindow window) =>
        controller.Connected && controller.ActiveProfileId == selected.Id && Array.IndexOf(remoteWindows, window) >= 0
            ? controller.ActivateWindowAsync(window.Id, expectedProfileId: selected.Id)
            : Task.CompletedTask;

    private void ResetConnectionView()
    {
        InterruptTransfers();
        remoteWindows = [];
        RenderWindows();
        backgroundList.Items.Clear();
        backgroundSnapshot = null;
        backgroundSection.Visibility = Visibility.Collapsed;
        UpdateDockVisibility();
        trayStatus.Text = "";
        touchApplicationNames.Clear();
        RefreshTouchApplications();
        actual.Text = diagnosticMediaPath.Text = "等待连接";
        appliedVideo.Text = "";
        appliedVideo.Visibility = Visibility.Collapsed;
        quickRtt.Text = "—";
        lastTick = lastBytes = lastUpdates = 0;
        foreach (var metric in metrics.Values) metric.Text = "等待采集";
    }

    private void UpdateLauncherActions()
    {
        bool hasProfile = profiles.Count > 0;
        bool live = controller.Connected && controller.ActiveProfileId == selected.Id;
        bool canPair = initialized && !busy && !controller.IsRunning;
        emptyLauncher.Visibility = hasProfile ? Visibility.Collapsed : Visibility.Visible;
        if (emptyLauncher.Parent is Border emptyCard) emptyCard.Visibility = emptyLauncher.Visibility;
        catalogScroll.Visibility = hasProfile ? Visibility.Visible : Visibility.Collapsed;
        emptyAdd.IsEnabled = canPair;
        refreshApps.IsEnabled = hasProfile && paired && !busy;
        applicationSearch.IsEnabled = hasProfile && paired;
        muteQuick.IsEnabled = readOnlyQuick.IsEnabled = endSessionQuick.IsEnabled = live && !busy;
        var requested = liveSettings ?? selected.Settings;
        string muteLabel = requested.Audio ? "静音" : "恢复声音";
        muteQuick.Content = muteLabel;
        AutomationProperties.SetName(muteQuick, muteLabel);
        string action = requested.ReadOnly ? "恢复远端输入" : "只读查看";
        readOnlyQuick.Content = Glyph(requested.ReadOnly ? "\uE785" : "\uE890");
        AutomationProperties.SetName(readOnlyQuick, action);
        ToolTipService.SetToolTip(readOnlyQuick, action);
        AutomationProperties.SetName(connectionDot, live ? "已连接" : controller.IsRunning ? "连接中" : "未连接");
        connectionDot.Fill = new SolidColorBrush(live ? ColorHelper.FromArgb(255, 22, 160, 93)
            : controller.IsRunning ? ColorHelper.FromArgb(255, 231, 173, 40) : ColorHelper.FromArgb(255, 126, 137, 153));
        sessionStatus.Text = live && controller.CurrentSession is { } current ? current : controller.IsRunning ? "连接中" : "未连接";
        UpdateDockVisibility();
        UpdateCommandControls();
        if (!hasProfile) status.Text = "尚未配对";
    }

    private async Task SelectProfileAsync(ConnectionProfile profile)
    {
        Load(profile);
        launcherPreferences.SelectedProfile = profile.Id;
        await SaveLauncherPreferencesAsync();
        if (profile.Id == selected.Id) await RefreshApplicationsAsync();
    }

    private async Task SaveLauncherPreferencesAsync()
    {
        await preferencesSave.WaitAsync();
        try { await launcherPreferences.SaveAsync(store.DirectoryPath); }
        catch (IOException error) { Inform("无法保存启动器偏好：" + error.Message); }
        finally { preferencesSave.Release(); }
    }

    private async Task RefreshApplicationsAsync()
    {
        var previousRequest = catalogLoad;
        catalogLoad = null;
        previousRequest?.Cancel();
        if (!paired || !profiles.Any(profile => profile.Id == selected.Id)) return;
        var profile = selected;
        string? credential = secrets.Read(profile.Id);
        if (credential is null) return;
        using var request = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        catalogLoad = request;
        SetCatalogStatus("正在读取应用目录…");
        try
        {
            var payload = await new DirectAgent(profile, credential).CallAsync(new { op = "applications" }, request.Token);
            if (!ReferenceEquals(catalogLoad, request) || selected.Id != profile.Id || request.IsCancellationRequested) return;
            var available = payload.Deserialize<List<RemoteApplication>>(Wire.Json) ?? [];
            SetCatalogStatus(available.Count > 0 ? "" : "工作站没有可启动的应用。");
            if (applications.Count != available.Count || !applications.Zip(available).All(pair => SameCatalogEntry(pair.First, pair.Second)))
            {
                applications = available;
                FilterApplications();
                RenderWindows();
                _ = GuardAsync(ApplyLauncherPoliciesAsync);
            }
        }
        catch (OperationCanceledException) when (ReferenceEquals(catalogLoad, request)) { SetCatalogStatus("读取目录超时，可以刷新重试。"); }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            if (ReferenceEquals(catalogLoad, request)) SetCatalogStatus("无法读取应用目录：" + error.Message);
        }
        finally { if (ReferenceEquals(catalogLoad, request)) catalogLoad = null; }
    }

    private void InvalidateCatalog()
    {
        var request = catalogLoad;
        catalogLoad = null;
        request?.Cancel();
        InvalidateIcons();
        applications = [];
        SetCatalogStatus("");
    }

    private void SetCatalogStatus(string text)
    {
        catalogStatus.Text = text;
        catalogStatus.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static bool SameCatalogEntry(RemoteApplication first, RemoteApplication second) =>
        first.Id == second.Id && first.Name == second.Name && first.Icon == second.Icon &&
        first.GenericName == second.GenericName && first.Comment == second.Comment && first.StartupWmClass == second.StartupWmClass &&
        first.Categories.SequenceEqual(second.Categories) && first.Keywords.SequenceEqual(second.Keywords) && first.Actions.SequenceEqual(second.Actions);

    private async Task QuickSettingAsync(bool audioSetting)
    {
        if (!controller.Connected || controller.ActiveProfileId != selected.Id) return;
        bool value = audioSetting ? !(liveSettings ?? selected.Settings).Audio : !(liveSettings ?? selected.Settings).ReadOnly;
        await ApplyQuickSettingsAsync(settings => audioSetting ? settings with { Audio = value } : settings with { ReadOnly = value },
            () => { if (audioSetting) audio.IsOn = value; else readOnly.IsOn = value; }, selected.Id);
    }

    private Task ApplyQuickSettingsAsync(Func<ClientSettings, ClientSettings> change, Action updateEditor, Guid profileId) =>
        RunOperationAsync(async () =>
        {
            if (!controller.Connected || selected.Id != profileId || controller.ActiveProfileId != profileId) return;
            var applied = liveSettings ?? selected.Settings;
            var updated = change(applied);
            var profile = selected with { Settings = change(selected.Settings) };
            await controller.ApplyAsync(updated, expectedProfileId: profile.Id);
            liveSettings = updated;
            await StoreProfileAsync(profile);
            updateEditor();
        });

    private Flyout AudioFlyout()
    {
        var quickVolume = new Slider { Minimum = 0, Maximum = 100, StepFrequency = 1, MinWidth = 220, Header = "远端音量" };
        var flyout = new Flyout { Content = Column(Heading("声音", 16), quickVolume, muteQuick) };
        bool opening = false;
        Guid target = Guid.Empty;
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(180);
        timer.IsRepeating = false;
        flyout.Opening += (_, _) =>
        {
            timer.Stop();
            opening = true;
            target = selected.Id;
            quickVolume.Value = (liveSettings ?? selected.Settings).Volume;
            quickVolume.IsEnabled = controller.Connected && controller.ActiveProfileId == target;
            opening = false;
        };
        quickVolume.ValueChanged += (_, _) => { if (!opening) { timer.Stop(); timer.Start(); } };
        timer.Tick += async (_, _) => await GuardAsync(() =>
        {
            int value = (int)quickVolume.Value;
            return ApplyQuickSettingsAsync(settings => settings with { Volume = value }, () => volume.Value = value, target);
        });
        return flyout;
    }

    private void MarkTransferSeen()
    {
        unreadTransfers = 0;
        transferBadge.Visibility = Visibility.Collapsed;
    }

    private void MarkTransferActivity()
    {
        if (transferDrawer.IsPaneOpen) return;
        transferBadge.Value = unreadTransfers = Math.Min(99, unreadTransfers + 1);
        transferBadge.Visibility = Visibility.Visible;
    }

    private UIElement GeneralPage()
    {
        theme.SelectionChanged += async (_, _) =>
        {
            if (loading) return;
            launcherPreferences.Theme = theme.SelectedIndex switch { 1 => "light", 2 => "dark", _ => "system" };
            ApplyLauncherTheme();
            await GuardAsync(SaveLauncherPreferencesAsync);
        };
        closeToTray.Toggled += async (_, _) =>
        {
            if (loading) return;
            launcherPreferences.CloseToTray = closeToTray.IsOn;
            await GuardAsync(SaveLauncherPreferencesAsync);
        };
        return Column(Heading("常规"), Muted("适用于此 Windows 客户端上的所有连接。"),
            Row("界面主题", "立即生效", theme), Row("关闭窗口时收起到托盘", "关闭此项后，关闭窗口会退出客户端并保留远端会话。", closeToTray),
            Row("搜索远端应用", "在控制中心内使用", Text("Ctrl + K")),
            Row("客户端架构", "", Text(System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString())));
    }
}
