using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.UI;
using Microsoft.UI.Composition;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
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
    private UiText catalogStatusMessage = "";
    private string catalogStatusDetail = "";
    private readonly TextBlock applicationsCount = Muted("");
    private readonly VariableSizedWrapGrid pinnedItems = Tiles();
    private readonly VariableSizedWrapGrid recentItems = Tiles();
    private readonly StackPanel pinnedSection = new() { Spacing = 6 };
    private readonly StackPanel favoritesSection = new() { Spacing = 12 };
    private readonly Border directoryHeader = new();
    private CompositionPropertySet? directoryMetrics;
    private bool directoryCompositionUnavailable;
    private readonly Grid catalogContent = new() { Margin = new Thickness(24, 16, 24, 20) };
    private readonly ScrollView catalogScroll = new()
    {
        HorizontalScrollMode = ScrollingScrollMode.Disabled, HorizontalScrollBarVisibility = ScrollingScrollBarVisibility.Hidden,
        VerticalScrollBarVisibility = ScrollingScrollBarVisibility.Auto, ZoomMode = ScrollingZoomMode.Disabled
    };
    private readonly Button matchingWindows = Button(L("Show matching windows"));
    private readonly TextBlock runningEmpty = Muted("");
    private readonly Grid dock = new() { ColumnSpacing = 12, RowSpacing = 10 };
    private readonly TextBlock pinHint = Muted(L("Pin favorite apps from their app menus."));
    private readonly TextBlock quickRtt = Muted("—");
    private readonly Ellipse connectionDot = new() { Width = 8, Height = 8, VerticalAlignment = VerticalAlignment.Center };
    private readonly ComboBox theme = Combo([L("Follow Windows"), L("Light"), L("Dark")]);
    private readonly ComboBox language = Combo([L("Follow Windows"), "English", "简体中文"]);
    private readonly ToggleSwitch closeToTray = Toggle(true);
    private readonly Button muteQuick = Button(L("Mute")), readOnlyQuick = IconButton(L("View only"), "\uE890");
    private readonly MenuFlyoutItem endSessionQuick = L("End remote session").Bind(new MenuFlyoutItem(), MenuFlyoutItem.TextProperty);
    private readonly Button modeButton = IconButton(L("Switch to compact list"), "\uE8FD");
    private readonly InfoBadge transferBadge = new() { Visibility = Visibility.Collapsed, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, IsHitTestVisible = false };
    private readonly SplitView transferDrawer = new()
    {
        Style = LauncherStyles.Transfers, PanePlacement = SplitViewPanePlacement.Right,
        DisplayMode = SplitViewDisplayMode.Overlay, OpenPaneLength = 390
    };
    private int unreadTransfers;
    private readonly LauncherCategoryPanel categories = new();
    private static readonly (UiText Name, string[] Tags)[] AppCategories =
    [
        (L("All"), []), (L("Graphics"), ["Graphics", "AudioVideo", "Video", "Audio"]),
        (L("Development"), ["Development"]), (L("Office"), ["Office", "Network", "Education"]),
        (L("Games"), ["Game"]), (L("System"), ["System", "Settings", "Utility"]), (L("Other"), [])
    ];
    private int category;
    private readonly StackLayout listLayout = new() { Spacing = 3 };
    private readonly UniformGridLayout gridLayout = new()
    {
        Orientation = Orientation.Horizontal, MinItemWidth = 154, MinItemHeight = AppTileCellHeight,
        ItemsStretch = UniformGridLayoutItemsStretch.Fill
    };
    private IReadOnlyList<RemoteApplication> filteredApplications = [];
    private readonly Button refreshApps = Button(L("Refresh catalog"));
    private readonly Button emptyAdd = Button(L("Add workstation"), true);
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

    private static Button IconButton(UiText label, string glyph)
    {
        var button = Button("");
        button.Content = Glyph(glyph);
        button.MinWidth = 36;
        button.Padding = new Thickness(8, 4, 8, 4);
        label.Name(button);
        return label.Bind(button, ToolTipService.ToolTipProperty);
    }

    private static VariableSizedWrapGrid Tiles() => new()
    { Orientation = Orientation.Horizontal, ItemWidth = 154, ItemHeight = AppTileCellHeight };

    private static TextBlock Muted(UiText text)
    {
        var label = Text(text, 12);
        label.Style = LauncherStyles.Muted;
        return label;
    }

    private static TextBlock Heading(UiText text, double size = 24)
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
        L("Select workstation").Name(profilePicker);
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
        var add = L("Add device").Bind(new MenuFlyoutItem { Icon = Glyph("\uE710") }, MenuFlyoutItem.TextProperty);
        add.Click += async (_, _) => await GuardAsync(PairDeviceAsync);
        var configure = L("Connection settings").Bind(new MenuFlyoutItem { Icon = Glyph("\uE713") }, MenuFlyoutItem.TextProperty);
        configure.Click += (_, _) => ShowPage("connection");
        muteQuick.Click += async (_, _) => await GuardAsync(() => QuickSettingAsync(audioSetting: true));
        readOnlyQuick.Click += async (_, _) => await GuardAsync(() => QuickSettingAsync(audioSetting: false));
        endSessionQuick.Click += async (_, _) => await GuardAsync(ConfirmEndAsync);
        foreach (MenuFlyoutItemBase item in new MenuFlyoutItemBase[] { add, configure, new MenuFlyoutSeparator(), endSessionQuick })
            menu.Items.Add(item);
        var more = IconButton(L("Manage workstations"), "\uE712");
        more.Flyout = menu;
        menu.Opening += (_, _) => add.IsEnabled = !busy && !controller.IsRunning;
        var hostControls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, VerticalAlignment = VerticalAlignment.Center,
            Children = { connectionDot, profilePicker, connect, more } };

        var audioButton = IconButton(L("Sound and volume"), "\uE767");
        audioButton.Flyout = AudioFlyout();
        var linkButton = IconButton(L("Connection quality"), "\uE839");
        linkButton.Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, Children = { Glyph("\uE839"), quickRtt } };
        var linkSummary = Text("");
        var diagnostics = Button(L("Open diagnostics"));
        var linkFlyout = new Flyout { Content = Column(Heading(L("Connection quality"), 16), linkSummary, diagnostics) };
        linkFlyout.Opening += (_, _) => linkSummary.Text = Tr("Round-trip latency · {0}\n{1}", quickRtt.Text, diagnosticMediaPath.Text);
        diagnostics.Click += (_, _) => { linkFlyout.Hide(); ShowPage("diagnostics"); };
        linkButton.Flyout = linkFlyout;
        var transfer = IconButton(L("File transfers"), "\uE8B7");
        transfer.Click += (_, _) => { transferDrawer.IsPaneOpen = !transferDrawer.IsPaneOpen; MarkTransferSeen(); };
        var transferAction = new Grid { Children = { transfer, transferBadge } };
        openTerminal.Click += async (_, _) => await GuardAsync(OpenRemoteTerminalAsync);
        var settings = IconButton(L("Settings"), "\uE713");
        settings.Click += (_, _) => ShowPage("display");
        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center,
            Children = { openTerminal, readOnlyQuick, audioButton, linkButton, transferAction, settings } };

        L("Search remote apps, windows, or commands").Name(applicationSearch);
        applicationSearch.TextChanged += (_, _) =>
        {
            if (applicationSearch.Text.Trim().Length > 0 && (CommandMode || ActivePage is not ("apps" or "running"))) ShowPage("apps");
            RefreshLauncherSearch();
        };
        applicationSearch.KeyDown += SearchKeyDown;
        foreach (var control in new Microsoft.UI.Xaml.Controls.Control[] { profilePicker, connect, more, applicationSearch, openTerminal, readOnlyQuick, audioButton, linkButton, transfer, settings })
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
        L("Remote app catalog").Name(applicationList);
        refreshApps.Click += async (_, _) => await GuardAsync(async () =>
        {
            InvalidateIcons();
            await RefreshApplicationsAsync();
            FilterApplications();
            if (controller.Connected && controller.ActiveProfileId == selected.Id) RenderWindows();
            else UpdateRemoteSessionView();
        });
        emptyAdd.Click += async (_, _) => await GuardAsync(PairDeviceAsync);

        var icon = Glyph("\uE7F4", 28);
        icon.HorizontalAlignment = HorizontalAlignment.Left;
        emptyLauncher.Children.Add(icon);
        emptyLauncher.Children.Add(Heading(L("Connect to your Linux workstation"), 22));
        emptyLauncher.Children.Add(Text(L("Pair to find and open apps on your workstation.")));
        emptyLauncher.Children.Add(emptyAdd);
        pinnedSection.Children.Add(Muted(L("Pinned"))); pinnedSection.Children.Add(pinnedItems);
        recentSection.Children.Add(Muted(L("Recently launched"))); recentSection.Children.Add(recentItems);
        favoritesSection.Children.Add(Heading(L("Pinned and recent"), 22));
        favoritesSection.Margin = new Thickness(0, 0, 0, 20);
        favoritesSection.Children.Add(pinHint);
        favoritesSection.Children.Add(pinnedSection);
        favoritesSection.Children.Add(recentSection);

        var directoryTitleBar = new Grid { ColumnSpacing = 8 };
        directoryTitleBar.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        directoryTitleBar.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        var directoryTitle = new StackPanel { Spacing = 2, Children = { Heading(L("All apps"), 19), applicationsCount } };
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
            UiText name = AppCategories[index].Name;
            var chip = new ToggleButton { Content = Text(name), IsChecked = index == 0, Padding = new Thickness(9, 4, 9, 4),
                HorizontalAlignment = HorizontalAlignment.Left };
            name.Name(chip);
            chip.Click += (_, _) => { category = selectedCategory; UpdateCategories(); FilterApplications(keepDirectoryAtTop: true); };
            categories.Children.Add(chip);
        }
        matchingWindows.Visibility = Visibility.Collapsed;
        matchingWindows.Click += (_, _) => ShowPage("running");
        catalogStatus.Visibility = Visibility.Collapsed;
        directoryHeader.Style = LauncherStyles.Directory;
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
        catalogScroll.Loaded += (_, _) => ScheduleDirectoryComposition();
        catalogScroll.SizeChanged += (_, _) => ScheduleDirectoryComposition();
        catalogScroll.ViewChanged += (_, _) => UpdateDirectoryFallback();
        favoritesSection.SizeChanged += (_, _) => UpdateDirectoryAnchor();
        var empty = Card(emptyLauncher, new Thickness(22));
        empty.Margin = new Thickness(24, 20, 24, 20);
        empty.VerticalAlignment = VerticalAlignment.Top;
        // Measure in the visual tree so WinUI applies the actual display and text scale.
        var titleMeasure = Heading("Ag国\nAg国", AppTitleFontSize);
        titleMeasure.TextWrapping = TextWrapping.NoWrap;
        titleMeasure.Opacity = 0;
        titleMeasure.IsHitTestVisible = false;
        titleMeasure.HorizontalAlignment = HorizontalAlignment.Left;
        titleMeasure.VerticalAlignment = VerticalAlignment.Top;
        AutomationProperties.SetAccessibilityView(titleMeasure, AccessibilityView.Raw);
        titleMeasure.SizeChanged += (_, args) =>
        {
            const double borderAndRounding = 5;
            double chrome = AppTileIconSize + AppTileTitleSpacing + AppTileTopPadding + AppTileBottomPadding + AppTileGap + borderAndRounding;
            double height = Math.Max(AppTileCellHeight, Math.Ceiling(args.NewSize.Height + chrome));
            gridLayout.MinItemHeight = pinnedItems.ItemHeight = recentItems.ItemHeight = height;
        };
        return new Grid { Children = { catalogScroll, empty, titleMeasure } };
    }

    private UIElement RunningPage()
    {
        L("Running windows").Name(windowList);
        L("Remote background apps").Name(backgroundList);
        var sessionCard = BuildRemoteSessionCard();
        windowSection.Children.Add(Heading(L("Remote windows"), 17)); windowSection.Children.Add(windowList);
        backgroundSection.Children.Add(Heading(L("Background apps"), 17)); backgroundSection.Children.Add(backgroundList);
        dock.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
        dock.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
        dock.RowDefinitions.Add(new() { Height = GridLength.Auto });
        dock.RowDefinitions.Add(new() { Height = GridLength.Auto });
        dock.Children.Add(windowSection);
        dock.Children.Add(backgroundSection);
        dock.SizeChanged += (_, args) => LayoutDock(args.NewSize.Width);
        windowSection.Visibility = backgroundSection.Visibility = Visibility.Collapsed;
        UpdateDockVisibility();
        return Column(Heading(L("Running"), 22), sessionCard, runningEmpty, dock, processSection);
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
        Layout layout = compact ? listLayout : gridLayout;
        if (applicationList.Layout != layout)
        {
            // The repeater can keep realized content when only its layout changes.
            applicationList.ItemsSource = null;
            applicationList.Layout = layout;
            applicationList.ItemsSource = filteredApplications;
        }
        modeButton.Content = Glyph(compact ? "\uE80A" : "\uE8FD");
        string label = compact ? Tr("Switch to grid") : Tr("Switch to compact list");
        AutomationProperties.SetName(modeButton, label);
        ToolTipService.SetToolTip(modeButton, label);
    }

    private double DirectoryTop => catalogContent.RowDefinitions[0].ActualHeight + catalogContent.Margin.Top;

    private void ScheduleDirectoryComposition()
    {
        if (directoryMetrics is null && !directoryCompositionUnavailable)
            DispatcherQueue.TryEnqueue(BindDirectoryHeaderToScroll);
    }

    private void BindDirectoryHeaderToScroll()
    {
        if (directoryMetrics is not null || directoryCompositionUnavailable || !directoryHeader.IsLoaded || catalogScroll.ActualHeight <= 0)
            return;

        try
        {
            // Loaded can run before the ScrollView template has a usable composition source.
            var scroll = catalogScroll.ScrollPresenter?.ExpressionAnimationSources;
            if (scroll is null) return;
            var compositor = scroll.Compositor;
            var metrics = compositor.CreatePropertySet();
            metrics.InsertScalar("HeaderTop", (float)DirectoryTop);
            using var pin = compositor.CreateExpressionAnimation("Vector3(0, Max(0, scroll.Position.Y - scroll.MinPosition.Y - layout.HeaderTop), 0)");
            pin.SetReferenceParameter("scroll", scroll);
            pin.SetReferenceParameter("layout", metrics);
            pin.Target = "Translation";
            directoryHeader.StartAnimation(pin);
            directoryMetrics = metrics;
        }
        catch (Exception error) when (error is ArgumentException or COMException)
        {
            directoryCompositionUnavailable = true;
            StartupDiagnostics.Write($"Directory header composition unavailable (0x{error.HResult:X8}); using scroll updates");
            UpdateDirectoryFallback();
        }
    }

    private void UpdateDirectoryAnchor()
    {
        if (directoryMetrics is { } metrics) metrics.InsertScalar("HeaderTop", (float)DirectoryTop);
        else UpdateDirectoryFallback();
    }

    private void UpdateDirectoryFallback()
    {
        if (directoryMetrics is null && catalogScroll.ScrollPresenter is not null)
            directoryHeader.Translation = new Vector3(0, (float)Math.Max(0, catalogScroll.VerticalOffset - DirectoryTop), 0);
    }

    private void ScrollCatalogTo(double offset) => catalogScroll.ScrollTo(0, offset,
        new ScrollingScrollOptions(ScrollingAnimationMode.Disabled, ScrollingSnapPointsMode.Ignore));

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
        bool hasDock = windowList.Items.Count > 0 || backgroundList.Items.Count > 0;
        bool hasProcesses = processList.Items.Count > 0;
        bool hasRunning = hasDock || hasProcesses;
        dock.Visibility = hasDock ? Visibility.Visible : Visibility.Collapsed;
        runningEmpty.Visibility = hasRunning ? Visibility.Collapsed : Visibility.Visible;
        bool live = controller.Connected && controller.ActiveProfileId == selected.Id;
        if (live)
            runningEmpty.Text = Tr("No matching remote windows or background apps.");
        else if (PrimaryRemoteSession is { } session)
            runningEmpty.Text = applicationSearch.Text.Trim().Length > 0
                ? Tr("No matching remote windows, background apps, or processes.")
                : session.DetailsAvailable
                    ? Tr("No open windows or background apps detected in session “{0}”.", session.Name)
                    : Tr("Update the Linux server to inspect open windows and background apps before connecting.");
        else
            runningEmpty.Text = "";
        if (runningEmpty.Text.Length == 0) runningEmpty.Visibility = Visibility.Collapsed;
    }

    private void FilterApplications(bool keepDirectoryAtTop = false)
    {
        bool scrollToDirectory = keepDirectoryAtTop && catalogScroll.VerticalOffset >= DirectoryTop;
        filteredApplications = applications.Where(app => MatchesApp(app) && MatchesCategory(app)).ToArray();
        applicationList.ItemsSource = filteredApplications;
        applicationList.Visibility = filteredApplications.Count > 0 && !CommandMode ? Visibility.Visible : Visibility.Collapsed;
        UpdateApplicationCount();
        RenderRecent();
        if (scrollToDirectory)
            DispatcherQueue.TryEnqueue(() => ScrollCatalogTo(Math.Min(DirectoryTop, catalogScroll.ScrollableHeight)));
    }

    private void UpdateApplicationCount()
    {
        applicationsCount.Text = applications.Count == 0 ? "" : filteredApplications.Count == 0 ? Tr("No matching apps") : Tr("{0} apps", filteredApplications.Count);
        applicationsCount.Visibility = applicationsCount.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
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
            L("Return to window: {0}", window.Title).Name(focus);
            focus.Click += async (_, _) => await GuardAsync(() => FocusRemoteWindowAsync(window));
            windowList.Items.Add(focus);
        }
        windowSection.Visibility = windowList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdateMatchingWindows();
        UpdateDockVisibility();
    }

    private void UpdateMatchingWindows()
    {
        matchingWindows.Visibility = !CommandMode && applicationSearch.Text.Trim().Length > 0 && windowList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        string label = Tr("Show {0} matching windows", windowList.Items.Count);
        matchingWindows.Content = label;
        AutomationProperties.SetName(matchingWindows, label);
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
        remoteTrayStatus = null;
        UpdateRemoteTrayStatus();
        touchApplicationNames.Clear();
        RefreshTouchApplications();
        actual.Text = diagnosticMediaPath.Text = Tr("Waiting for connection");
        appliedVideo.Text = "";
        appliedVideo.Visibility = Visibility.Collapsed;
        quickRtt.Text = "—";
        lastTick = lastBytes = lastUpdates = 0;
        foreach (var metric in metrics.Values) metric.Text = Tr("Waiting for capture");
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
        openTerminal.IsEnabled = initialized && hasProfile && paired && !busy;
        applicationSearch.IsEnabled = hasProfile && paired;
        muteQuick.IsEnabled = readOnlyQuick.IsEnabled = live && !busy;
        endSessionQuick.IsEnabled = CanEndRemoteSession;
        var requested = liveSettings ?? selected.Settings;
        string muteLabel = requested.Audio ? Tr("Mute") : Tr("Unmute");
        muteQuick.Content = muteLabel;
        AutomationProperties.SetName(muteQuick, muteLabel);
        string action = requested.ReadOnly ? Tr("Resume remote input") : Tr("View only");
        readOnlyQuick.Content = Glyph(requested.ReadOnly ? "\uE785" : "\uE890");
        AutomationProperties.SetName(readOnlyQuick, action);
        ToolTipService.SetToolTip(readOnlyQuick, action);
        AutomationProperties.SetName(connectionDot, live ? Tr("Connected") : controller.IsRunning ? Tr("Connecting") : Tr("Disconnected"));
        connectionDot.Fill = new SolidColorBrush(live ? ColorHelper.FromArgb(255, 22, 160, 93)
            : controller.IsRunning ? ColorHelper.FromArgb(255, 231, 173, 40) : ColorHelper.FromArgb(255, 126, 137, 153));
        sessionStatus.Text = live && controller.CurrentSession is { } current ? current
            : controller.IsRunning ? Tr("Connecting")
            : PrimaryRemoteSession is { } remote ? Tr("Remote session running · {0}", remote.Name)
            : remoteSessionsChecked && remoteSessionsError.Length == 0 && paired ? Tr("No remote session")
            : Tr("Disconnected");
        UpdateRemoteSessionView();
        UpdateCommandControls();
        if (!hasProfile) status.Text = Tr("Unpaired");
    }

    private async Task SelectProfileAsync(ConnectionProfile profile)
    {
        Load(profile);
        launcherPreferences.SelectedProfile = profile.Id;
        await SaveLauncherPreferencesAsync();
        if (profile.Id == selected.Id)
        {
            _ = GuardAsync(() => RefreshRemoteSessionsAsync(quiet: true));
            await RefreshApplicationsAsync();
        }
    }

    private async Task SaveLauncherPreferencesAsync()
    {
        await preferencesSave.WaitAsync();
        try { await launcherPreferences.SaveAsync(store.DirectoryPath); }
        catch (IOException error) { Inform(Tr("Could not save launcher preferences: ") + error.Message); }
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
        SetCatalogStatus(L("Loading the app catalog…"));
        try
        {
            var payload = await new DirectAgent(profile, credential).CallAsync(new { op = "applications" }, request.Token);
            if (!ReferenceEquals(catalogLoad, request) || selected.Id != profile.Id || request.IsCancellationRequested) return;
            var available = payload.Deserialize<List<RemoteApplication>>(Wire.Json) ?? [];
            SetCatalogStatus(available.Count > 0 ? "" : L("No launchable apps found on the workstation."));
            if (applications.Count != available.Count || !applications.Zip(available).All(pair => SameCatalogEntry(pair.First, pair.Second)))
            {
                applications = available;
                FilterApplications();
                if (controller.Connected && controller.ActiveProfileId == selected.Id) RenderWindows();
                else UpdateRemoteSessionView();
                _ = GuardAsync(ApplyLauncherPoliciesAsync);
            }
        }
        catch (OperationCanceledException) when (ReferenceEquals(catalogLoad, request)) { SetCatalogStatus(L("Catalog request timed out. Refresh to retry.")); }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            if (ReferenceEquals(catalogLoad, request)) SetCatalogStatus(L("Could not load the app catalog: "), error.Message);
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

    private void SetCatalogStatus(UiText message, string detail = "")
    {
        catalogStatusMessage = message;
        catalogStatusDetail = detail;
        UpdateCatalogStatus();
    }

    private void UpdateCatalogStatus()
    {
        catalogStatus.Text = catalogStatusMessage.Resolve() + catalogStatusDetail;
        catalogStatus.Visibility = catalogStatus.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateRemoteTrayStatus()
    {
        trayStatus.Text = remoteTrayStatus is not { } message ? Tr("Tray: Waiting for connection")
            : TouchFlag(message, "available") == true ? Tr("SNI tray connected. Legacy XEmbed tray icons are unsupported.")
            : Tr("Tray unavailable: ") + TrDiagnostic(message.TryGetProperty("message", out var detail)
                ? detail.GetString() ?? "Unknown reason" : "Unknown reason");
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
        var quickVolume = L("Remote volume").Bind(new Slider { Minimum = 0, Maximum = 100, StepFrequency = 1, MinWidth = 220 }, Slider.HeaderProperty);
        var flyout = new Flyout { Content = Column(Heading(L("Sound"), 16), quickVolume, muteQuick) };
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
            await GuardAsync(async () =>
            {
                await SaveLauncherPreferencesAsync();
                await SyncAppearanceAsync();
            });
        };
        language.SelectionChanged += async (_, _) =>
        {
            if (loading || language.SelectedIndex < 0) return;
            string preference = language.SelectedIndex switch { 1 => "en", 2 => "zh-Hans", _ => "system" };
            await GuardAsync(() => ApplyLanguageAsync(preference));
        };
        closeToTray.Toggled += async (_, _) =>
        {
            if (loading) return;
            launcherPreferences.CloseToTray = closeToTray.IsOn;
            await GuardAsync(SaveLauncherPreferencesAsync);
        };
        return Column(Heading(L("General")), Muted(L("Applies to every connection on this Windows client.")),
            Row(L("Appearance"), L("Applies immediately to Waybridge and supported remote apps."), theme),
            Row(L("Display language"), L("Applies immediately"), language),
            Row(L("Minimize to tray on close"), L("Turn this off to exit the client when closing the window. The remote session stays running."), closeToTray),
            Row(L("Search remote apps"), L("Available in the control center"), Text("Ctrl + K")),
            Row(L("Client architecture"), "", Text(System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString())));
    }
}
