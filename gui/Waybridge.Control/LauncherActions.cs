using System.Text;
using System.Text.Json;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Waybridge.Control.Core;
using Windows.System;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private const double AppTitleLineHeight = 20;
    private const double AppTileCellHeight = 122;
    private readonly TextBox commandDirectory = new() { PlaceholderText = "使用 Linux 用户主目录" };
    private readonly Button runCommand = Button("运行", true);
    private readonly TextBlock commandTarget = Muted("");
    private readonly StackPanel commandHistory = new() { Spacing = 5 };
    private readonly StackPanel commandPane = new() { Visibility = Visibility.Collapsed, Spacing = 12 };
    private LaunchCommand? suggestedCommand;
    private bool launcherTouchPending;
    private bool CommandMode => applicationSearch.Text.TrimStart().StartsWith('>');

    private UIElement AppTile(RemoteApplication app, bool compact)
    {
        var label = Heading(app.Name, 14);
        label.MaxLines = 2;
        label.TextTrimming = TextTrimming.CharacterEllipsis;
        label.LineHeight = AppTitleLineHeight;
        label.LineStackingStrategy = LineStackingStrategy.BlockLineHeight;
        var mark = LauncherMark(app, compact ? 28 : 43);
        var content = new Grid { ColumnSpacing = 10, RowSpacing = 6, Children = { mark } };
        if (compact)
        {
            content.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
            content.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            label.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(label, 1);
        }
        else
        {
            content.RowDefinitions.Add(new() { Height = new GridLength(43) });
            content.RowDefinitions.Add(new() { Height = new GridLength(2 * AppTitleLineHeight) });
            label.VerticalAlignment = VerticalAlignment.Top;
            Grid.SetRow(label, 1);
        }
        mark.HorizontalAlignment = HorizontalAlignment.Left;
        mark.VerticalAlignment = VerticalAlignment.Center;
        content.Children.Add(label);
        var launch = Button(app.Name);
        launch.Content = content;
        launch.Padding = compact ? new Thickness(12, 3, 42, 3) : new Thickness(12, 10, 25, 9);
        launch.HorizontalAlignment = HorizontalAlignment.Stretch;
        launch.VerticalAlignment = VerticalAlignment.Stretch;
        launch.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        launch.VerticalContentAlignment = compact ? VerticalAlignment.Center : VerticalAlignment.Top;
        launch.Click += async (_, _) => await GuardAsync(() => LaunchApplicationAsync(app));

        var more = IconButton($"{app.Name} 的操作", "\uE712");
        more.Height = more.MinHeight = more.Width = more.MinWidth = 28;
        more.Padding = new Thickness(4);
        if (Application.Current.Resources.TryGetValue("SubtleButtonStyle", out var moreStyle)) more.Style = (Style)moreStyle;
        more.HorizontalAlignment = HorizontalAlignment.Right;
        more.VerticalAlignment = VerticalAlignment.Top;
        more.Margin = new Thickness(0, 5, 5, 0);
        var menu = ApplicationMenu(app);
        launch.ContextFlyout = menu;
        more.Flyout = menu;
        var tile = new Grid { Margin = compact ? new Thickness(0, 1, 0, 1) : new Thickness(0, 0, 7, 7),
            MinHeight = compact ? 2 * AppTitleLineHeight + 8 : AppTileCellHeight - 7, Children = { launch, more } };
        ToolTipService.SetToolTip(launch, string.IsNullOrWhiteSpace(app.Comment) ? app.Name : $"{app.Name}\n{app.Comment}");
        return tile;
    }

    private MenuFlyout ApplicationMenu(RemoteApplication app)
    {
        var menu = new MenuFlyout();
        var profileId = selected.Id;
        MenuFlyoutItem ActionItem(string label, Func<Task> action)
        {
            var item = new MenuFlyoutItem { Text = label };
            item.Click += async (_, _) => await GuardAsync(() => selected.Id == profileId ? action() : Task.CompletedTask);
            return item;
        }

        menu.Opening += (_, _) =>
        {
            menu.Items.Clear();
            menu.Items.Add(ActionItem("启动", () => LaunchApplicationAsync(app)));
            foreach (var action in app.Actions)
                menu.Items.Add(ActionItem(action.Name, () => LaunchApplicationAsync(app, action.Id)));
            menu.Items.Add(new MenuFlyoutSeparator());
            bool pinned = launcherPreferences.Pinned.GetValueOrDefault(profileId)?.Any(value => value.Id == app.Id) == true;
            menu.Items.Add(ActionItem(pinned ? "取消固定" : "固定到顶部", () => TogglePinAsync(app)));
            var shortcuts = new MenuFlyoutSubItem { Text = "应用链接与快捷方式", IsEnabled = paired };
            shortcuts.Items.Add(ActionItem("复制应用链接", () => CopyApplicationLinkAsync(app)));
            shortcuts.Items.Add(ActionItem("添加到开始菜单", () => SaveApplicationShortcutAsync(app, desktop: false)));
            shortcuts.Items.Add(ActionItem("创建桌面快捷方式", () => SaveApplicationShortcutAsync(app, desktop: true)));
            menu.Items.Add(shortcuts);
            var touch = new MenuFlyoutSubItem { Text = "触控模式" };
            string mode = launcherPreferences.AppTouchModes.GetValueOrDefault(profileId)?.GetValueOrDefault(app.Id) ?? "";
            foreach (var (value, label) in new[] { ("", "自动选择"), ("native", "优先原生触控"), ("mouse", "鼠标兼容") })
            {
                var item = new ToggleMenuFlyoutItem { Text = label, IsChecked = mode == value };
                item.Click += async (_, _) => await GuardAsync(() => selected.Id == profileId ? SetLauncherTouchAsync(app, value) : Task.CompletedTask);
                touch.Items.Add(item);
            }
            menu.Items.Add(touch);
            menu.Items.Add(ActionItem("自定义启动…", () => CustomLaunchAsync(app)));
        };
        return menu;
    }

    private async Task TogglePinAsync(RemoteApplication app)
    {
        if (!launcherPreferences.Pinned.TryGetValue(selected.Id, out var pins))
            launcherPreferences.Pinned[selected.Id] = pins = [];
        if (pins.RemoveAll(item => item.Id == app.Id) == 0)
            pins.Add(new(app.Id, app.Name, app.Icon));
        RenderRecent();
        await SaveLauncherPreferencesAsync();
    }

    private RemoteApplication? MatchWindowApplication(string id)
    {
        if (id.Length == 0) return null;
        var exact = applications.FirstOrDefault(app => app.Id == id || app.Id == id + ".desktop");
        if (exact is not null) return exact;
        var candidates = applications.Where(app => app.StartupWmClass.Length > 0
            && string.Equals(app.StartupWmClass, id, StringComparison.OrdinalIgnoreCase)).Take(2).ToArray();
        return candidates.Length == 1 ? candidates[0] : null;
    }

    private ClientSettings SettingsWithLauncherTouch(ClientSettings settings)
    {
        var preferred = launcherPreferences.AppTouchModes.GetValueOrDefault(selected.Id);
        if (preferred is null || preferred.Count == 0) return settings;
        var mapped = new Dictionary<string, string>(settings.TouchModes, StringComparer.Ordinal);
        foreach (var window in remoteWindows)
        {
            if (mapped.Count >= 100) break;
            if (MatchWindowApplication(window.AppId) is { } app && preferred.TryGetValue(app.Id, out var mode)
                && mode is "native" or "mouse")
                mapped.TryAdd(window.AppId, mode);
        }
        return settings with { TouchModes = mapped };
    }

    private async Task SetLauncherTouchAsync(RemoteApplication app, string value)
    {
        if (!launcherPreferences.AppTouchModes.TryGetValue(selected.Id, out var modes))
            launcherPreferences.AppTouchModes[selected.Id] = modes = new(StringComparer.Ordinal);
        if (value.Length == 0) modes.Remove(app.Id);
        else modes[app.Id] = value;
        await SaveLauncherPreferencesAsync();
        await ApplyLauncherTouchAsync();
        if (!remoteWindows.Any(window => MatchWindowApplication(window.AppId)?.Id == app.Id))
            Inform("已保存触控策略，识别到该应用的窗口后应用。");
    }

    private Task ApplyLauncherTouchAsync()
    {
        launcherTouchPending = false;
        if (!controller.Connected || controller.ActiveProfileId != selected.Id || liveSettings is null) return Task.CompletedTask;
        if (busy) { launcherTouchPending = true; return Task.CompletedTask; }
        var requested = SettingsWithLauncherTouch(liveSettings with { TouchModes = selected.Settings.TouchModes });
        if (requested.TouchModes.Count == liveSettings.TouchModes.Count
            && requested.TouchModes.All(pair => liveSettings.TouchModes.GetValueOrDefault(pair.Key) == pair.Value))
            return Task.CompletedTask;
        var profileId = selected.Id;
        return RunOperationAsync(async () =>
        {
            await controller.ApplyAsync(requested, expectedProfileId: profileId);
            liveSettings = requested;
        });
    }

    private async Task<bool> ConnectForLaunchAsync(ConnectionProfile profile)
    {
        if (!controller.Connected)
        {
            if (controller.IsRunning) throw new InvalidOperationException("会话正在重连，连接恢复后可启动应用");
            await ConnectProfileAsync(profile);
        }
        return selected.Id == profile.Id && controller.ActiveProfileId == profile.Id && controller.Connected;
    }

    private Task LaunchApplicationAsync(RemoteApplication app, string? action = null) => RunOperationAsync(async () =>
    {
        var profile = selected;
        if (!await ConnectForLaunchAsync(profile)) return;
        await LaunchForProfileAsync(profile, app.Id, app, action);
    }, canCancelConnect: true);

    private async Task LaunchForProfileAsync(ConnectionProfile profile, string appId, RemoteApplication? app,
                                             string? action = null, bool shellActivation = false)
    {
        await controller.LaunchAsync(app: appId, action: action, expectedProfileId: profile.Id, shellActivation: shellActivation);
        if (app is not null) await RememberLaunchAsync(profile, app);
    }

    private async Task RememberLaunchAsync(ConnectionProfile profile, RemoteApplication app)
    {
        if (!launcherPreferences.Recent.TryGetValue(profile.Id, out var recent))
            launcherPreferences.Recent[profile.Id] = recent = [];
        recent.RemoveAll(item => item.Id == app.Id);
        recent.Insert(0, new(app.Id, app.Name, app.Icon));
        if (recent.Count > 12) recent.RemoveRange(12, recent.Count - 12);
        RenderRecent();
        await SaveLauncherPreferencesAsync();
    }

    private Task CustomLaunchAsync(RemoteApplication app) => RunOperationAsync(async () =>
    {
        var profile = selected;
        var cwdField = new TextBox { PlaceholderText = "使用应用默认目录" };
        var arguments = new TextBox { PlaceholderText = "例如 --profile \"工作配置\"" };
        var exec = new CheckBox { Content = "用命令启动（不使用桌面激活）" };
        string[]? extra = null;
        string? cwd = null;
        bool edit = await PromptAsync($"自定义启动 · {app.Name}", "预览",
            Column(Field("额外参数", arguments), Field("Linux 工作目录", cwdField), exec), () =>
            {
                extra = string.IsNullOrWhiteSpace(arguments.Text) ? null : ParseLaunchArguments(arguments.Text);
                cwd = OptionalDirectory(cwdField.Text);
            });
        if (!edit || !await ConnectForLaunchAsync(profile)) return;
        bool directExec = exec.IsChecked == true;
        var preview = await controller.PreviewLaunchAsync(app: app.Id, cwd: cwd, extraArgv: extra,
            launchWithExec: directExec, expectedProfileId: profile.Id);
        string command = preview.TryGetProperty("exec", out var value) ? value.GetString() ?? "桌面激活" : "桌面激活";
        string directory = preview.TryGetProperty("cwd", out var path) ? path.GetString() ?? "" : "";
        if (!await ConfirmAsync($"启动 {app.Name}？", $"{command}\n\n工作目录 · {directory}", "启动")) return;
        await controller.LaunchAsync(app: app.Id, cwd: cwd, extraArgv: extra, launchWithExec: directExec, expectedProfileId: profile.Id);
        await RememberLaunchAsync(profile, app);
    }, canCancelConnect: true);

    private StackPanel BuildCommandPane()
    {
        commandPane.Children.Add(Heading("运行命令", 23));
        commandPane.Children.Add(commandTarget);
        AutomationProperties.SetName(commandDirectory, "Linux 工作目录");
        commandPane.Children.Add(Row("工作目录", "可选，填写 Linux 上的绝对路径。", commandDirectory));
        commandPane.Children.Add(runCommand);
        commandPane.Children.Add(commandHistory);
        runCommand.Click += async (_, _) => await GuardAsync(RunCommandAsync);
        return commandPane;
    }

    private void RefreshLauncherSearch()
    {
        commandPane.Visibility = CommandMode ? Visibility.Visible : Visibility.Collapsed;
        directoryHeader.Visibility = CommandMode ? Visibility.Collapsed : Visibility.Visible;
        if (!CommandMode) FilterApplications();
        else favoritesSection.Visibility = applicationList.Visibility = Visibility.Collapsed;
        RenderWindows();
        UpdateCommandControls();
        if (CommandMode) RefreshCommandHistory();
        if (ActivePage == "apps") catalogScroll.ChangeView(null, 0, null, true);
        UpdateDirectoryStickiness();
    }

    private void UpdateCommandControls()
    {
        commandTarget.Text = $"在 {selected.Name} 执行。用引号包住有空格的参数。";
        runCommand.IsEnabled = !busy && paired && CommandMode && CommandText().Length > 0;
    }

    private string CommandText()
    {
        string text = applicationSearch.Text.TrimStart();
        return text.StartsWith('>') ? text[1..].Trim() : "";
    }

    private void RefreshCommandHistory()
    {
        commandHistory.Children.Clear();
        var history = launcherPreferences.Commands.GetValueOrDefault(selected.Id) ?? [];
        string query = CommandText();
        var matches = history.Where(item => item.Text.StartsWith(query, StringComparison.OrdinalIgnoreCase)).Take(5).ToArray();
        suggestedCommand = matches.FirstOrDefault();
        if (matches.Length == 0) return;
        commandHistory.Children.Add(Muted("最近命令 · Tab 补全首条"));
        foreach (var command in matches)
        {
            var use = Button(command.Text);
            use.Click += (_, _) => UseCommand(command);
            commandHistory.Children.Add(use);
        }
    }

    private void UseCommand(LaunchCommand command)
    {
        commandDirectory.Text = command.Cwd ?? "";
        applicationSearch.Text = "> " + command.Text;
        applicationSearch.Select(applicationSearch.Text.Length, 0);
        applicationSearch.Focus(FocusState.Keyboard);
    }

    private async void SearchKeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key == VirtualKey.Escape && applicationSearch.Text.Length > 0)
        {
            applicationSearch.Text = "";
            args.Handled = true;
        }
        else if (args.Key == VirtualKey.Tab && CommandMode && suggestedCommand is { } suggestion
            && suggestion.Text != CommandText())
        {
            UseCommand(suggestion);
            args.Handled = true;
        }
        else if (args.Key == VirtualKey.Enter && applicationSearch.Text.Trim().Length > 0)
        {
            args.Handled = true;
            if (CommandMode) await GuardAsync(RunCommandAsync);
            else
            {
                if (ActivePage == "apps" && filteredApplications.FirstOrDefault() is { } app)
                    await GuardAsync(() => LaunchApplicationAsync(app));
                else if (remoteWindows.FirstOrDefault(value => Matches(value.Title, value.AppId)) is { } window)
                    await GuardAsync(() => FocusRemoteWindowAsync(window));
            }
        }
    }

    private Task RunCommandAsync() => RunOperationAsync(async () =>
    {
        var profile = selected;
        string text = CommandText();
        var argv = ParseLaunchArguments(text);
        string? cwd = OptionalDirectory(commandDirectory.Text);
        if (!await ConnectForLaunchAsync(profile)) return;
        await controller.LaunchAsync(argv: argv, cwd: cwd, expectedProfileId: profile.Id);
        if (!launcherPreferences.Commands.TryGetValue(profile.Id, out var history))
            launcherPreferences.Commands[profile.Id] = history = [];
        history.RemoveAll(command => command.Text == text && command.Cwd == cwd);
        history.Insert(0, new(text, cwd));
        if (history.Count > 20) history.RemoveRange(20, history.Count - 20);
        RefreshCommandHistory();
        await SaveLauncherPreferencesAsync();
    }, canCancelConnect: true);

    private static string? OptionalDirectory(string text)
    {
        text = text.Trim();
        if (text.Length == 0) return null;
        if (!text.StartsWith('/')) throw new ArgumentException("请输入 Linux 上的绝对工作目录");
        return text;
    }

    private static string[] ParseLaunchArguments(string text)
    {
        var args = new List<string>();
        var current = new StringBuilder();
        char quote = '\0';
        bool hasWord = false;
        bool escaped = false;
        foreach (char value in text)
        {
            if (escaped) { current.Append(value); escaped = false; continue; }
            if (value == '\\' && quote != '\'') { hasWord = true; escaped = true; continue; }
            if (quote != '\0')
            {
                if (value == quote) quote = '\0'; else current.Append(value);
                continue;
            }
            if (value is '\'' or '"') { hasWord = true; quote = value; continue; }
            if (char.IsWhiteSpace(value))
            {
                if (hasWord) { args.Add(current.ToString()); current.Clear(); hasWord = false; }
                continue;
            }
            hasWord = true;
            current.Append(value);
        }
        if (escaped || quote != '\0') throw new ArgumentException("命令的引号或转义符未闭合");
        if (hasWord) args.Add(current.ToString());
        if (args.Count == 0) throw new ArgumentException("请输入要运行的命令或参数");
        return args.ToArray();
    }
}
