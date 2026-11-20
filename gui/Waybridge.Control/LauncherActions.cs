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
    private const double AppTitleFontSize = 14;
    private const double AppTileCellHeight = 122;
    private const double AppTileIconSize = 43;
    private const double AppTileTitleSpacing = 6;
    private const double AppTileTopPadding = 10;
    private const double AppTileBottomPadding = 9;
    private const double AppTileGap = 7;
    private readonly TextBox commandDirectory = L("Use the Linux home directory").Bind(new TextBox(), TextBox.PlaceholderTextProperty);
    private readonly Button runCommand = Button(L("Run"), true);
    private readonly TextBlock commandTarget = Muted("");
    private readonly StackPanel commandHistory = new() { Spacing = 5 };
    private readonly StackPanel commandPane = new() { Visibility = Visibility.Collapsed, Spacing = 12 };
    private LaunchCommand? suggestedCommand;
    private bool launcherPoliciesPending;
    private bool CommandMode => applicationSearch.Text.TrimStart().StartsWith('>');

    private UIElement AppTile(RemoteApplication app, bool compact)
    {
        var label = Heading(app.Name, AppTitleFontSize);
        label.MaxLines = compact ? 1 : 2;
        label.TextWrapping = compact ? TextWrapping.NoWrap : TextWrapping.Wrap;
        label.TextTrimming = TextTrimming.CharacterEllipsis;
        var mark = LauncherMark(app, compact ? 28 : AppTileIconSize);
        var content = new Grid { ColumnSpacing = 10, RowSpacing = AppTileTitleSpacing, Children = { mark } };
        if (compact)
        {
            content.ColumnDefinitions.Add(new() { Width = GridLength.Auto });
            content.ColumnDefinitions.Add(new() { Width = new GridLength(1, GridUnitType.Star) });
            label.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(label, 1);
        }
        else
        {
            content.RowDefinitions.Add(new() { Height = new GridLength(AppTileIconSize) });
            content.RowDefinitions.Add(new() { Height = GridLength.Auto });
            label.VerticalAlignment = VerticalAlignment.Top;
            Grid.SetRow(label, 1);
        }
        mark.HorizontalAlignment = HorizontalAlignment.Left;
        mark.VerticalAlignment = VerticalAlignment.Center;
        content.Children.Add(label);
        var launch = Button(app.Name);
        launch.Content = content;
        launch.Padding = compact ? new Thickness(12, 3, 42, 3) : new Thickness(12, AppTileTopPadding, 25, AppTileBottomPadding);
        launch.HorizontalAlignment = HorizontalAlignment.Stretch;
        launch.VerticalAlignment = VerticalAlignment.Stretch;
        launch.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        launch.VerticalContentAlignment = compact ? VerticalAlignment.Center : VerticalAlignment.Top;
        launch.Click += async (_, _) => await GuardAsync(() => LaunchApplicationAsync(app));

        var more = IconButton(L("Actions for {0}", app.Name), "\uE712");
        more.Height = more.MinHeight = more.Width = more.MinWidth = 28;
        more.Padding = new Thickness(4);
        if (Application.Current.Resources.TryGetValue("SubtleButtonStyle", out var moreStyle)) more.Style = (Style)moreStyle;
        more.HorizontalAlignment = HorizontalAlignment.Right;
        more.VerticalAlignment = VerticalAlignment.Top;
        more.Margin = new Thickness(0, 5, 5, 0);
        var menu = ApplicationMenu(app);
        launch.ContextFlyout = menu;
        more.Flyout = menu;
        var tile = new Grid { Margin = compact ? new Thickness(0, 1, 0, 1) : new Thickness(0, 0, AppTileGap, AppTileGap),
            MinHeight = compact ? 48 : AppTileCellHeight - AppTileGap, Children = { launch, more } };
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
            menu.Items.Add(ActionItem(Tr("Launch"), () => LaunchApplicationAsync(app)));
            foreach (var action in app.Actions)
                menu.Items.Add(ActionItem(action.Name, () => LaunchApplicationAsync(app, action.Id)));
            menu.Items.Add(new MenuFlyoutSeparator());
            bool pinned = launcherPreferences.Pinned.GetValueOrDefault(profileId)?.Any(value => value.Id == app.Id) == true;
            menu.Items.Add(ActionItem(pinned ? Tr("Unpin") : Tr("Pin to top"), () => TogglePinAsync(app)));
            var shortcuts = new MenuFlyoutSubItem { Text = Tr("App links and shortcuts"), IsEnabled = paired };
            shortcuts.Items.Add(ActionItem(Tr("Copy app link"), () => CopyApplicationLinkAsync(app)));
            shortcuts.Items.Add(ActionItem(Tr("Add to Start menu"), () => SaveApplicationShortcutAsync(app, desktop: false)));
            shortcuts.Items.Add(ActionItem(Tr("Create desktop shortcut"), () => SaveApplicationShortcutAsync(app, desktop: true)));
            menu.Items.Add(shortcuts);
            var touch = new MenuFlyoutSubItem { Text = Tr("Touch mode") };
            string mode = launcherPreferences.AppTouchModes.GetValueOrDefault(profileId)?.GetValueOrDefault(app.Id) ?? "";
            foreach (var (value, label) in new[] { ("", Tr("Automatic")), ("native", Tr("Prefer native touch")), ("mouse", Tr("Mouse compatibility")) })
            {
                var item = new ToggleMenuFlyoutItem { Text = label, IsChecked = mode == value };
                item.Click += async (_, _) => await GuardAsync(() => selected.Id == profileId ? SetLauncherTouchAsync(app, value) : Task.CompletedTask);
                touch.Items.Add(item);
            }
            menu.Items.Add(touch);
            var stream = new MenuFlyoutSubItem { Text = Tr("Streaming preset") };
            string preset = launcherPreferences.AppStreamModes.GetValueOrDefault(profileId)?.GetValueOrDefault(app.Id) ?? "";
            foreach (var (value, label) in new[] { ("", Tr("Use connection default")), ("clear", Tr("Crisp text")), ("low_latency", Tr("Low latency")) })
            {
                var item = new ToggleMenuFlyoutItem { Text = label, IsChecked = preset == value };
                item.Click += async (_, _) => await GuardAsync(() => selected.Id == profileId ? SetAppStreamModeAsync(app, value) : Task.CompletedTask);
                stream.Items.Add(item);
            }
            menu.Items.Add(stream);
            menu.Items.Add(ActionItem(Tr("Custom launch…"), () => CustomLaunchAsync(app)));
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

    private ClientSettings SettingsWithLauncherPolicies(ClientSettings settings)
    {
        var preferred = launcherPreferences.AppTouchModes.GetValueOrDefault(selected.Id);
        var mapped = new Dictionary<string, string>(settings.TouchModes, StringComparer.Ordinal);
        if (preferred is { Count: > 0 })
        {
            foreach (var window in remoteWindows)
            {
                if (mapped.Count >= 100) break;
                if (MatchWindowApplication(window.AppId) is { } app && preferred.TryGetValue(app.Id, out var mode)
                    && mode is "native" or "mouse")
                    mapped.TryAdd(window.AppId, mode);
            }
        }
        return settings with { TouchModes = mapped, AppStreamModes = ApplicationStreamModes() };
    }

    private Dictionary<string, string> ApplicationStreamModes()
    {
        var modes = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!launcherPreferences.AppStreamModes.TryGetValue(selected.Id, out var saved) || saved is null) return modes;
        foreach (var (id, mode) in saved.Take(128))
            if (mode is "clear" or "low_latency") modes[id] = mode;
        foreach (var window in remoteWindows)
        {
            if (modes.Count >= 256) break;
            if (MatchWindowApplication(window.AppId) is { } app && modes.TryGetValue(app.Id, out var mode))
                modes.TryAdd(window.AppId, mode);
        }
        return modes;
    }

    private async Task SetAppStreamModeAsync(RemoteApplication app, string value)
    {
        if (!launcherPreferences.AppStreamModes.TryGetValue(selected.Id, out var modes))
            launcherPreferences.AppStreamModes[selected.Id] = modes = new(StringComparer.Ordinal);
        if (value.Length == 0) modes.Remove(app.Id);
        else
        {
            if (modes.Count >= 128 && !modes.ContainsKey(app.Id))
                throw new InvalidOperationException(Tr("You can save streaming presets for up to 128 apps per workstation"));
            modes[app.Id] = value;
        }
        if (modes.Count == 0) launcherPreferences.AppStreamModes.Remove(selected.Id);
        await SaveLauncherPreferencesAsync();
        await ApplyLauncherPoliciesAsync();
    }

    private async Task SetLauncherTouchAsync(RemoteApplication app, string value)
    {
        if (!launcherPreferences.AppTouchModes.TryGetValue(selected.Id, out var modes))
            launcherPreferences.AppTouchModes[selected.Id] = modes = new(StringComparer.Ordinal);
        if (value.Length == 0) modes.Remove(app.Id);
        else modes[app.Id] = value;
        await SaveLauncherPreferencesAsync();
        await ApplyLauncherPoliciesAsync();
        if (!remoteWindows.Any(window => MatchWindowApplication(window.AppId)?.Id == app.Id))
            Inform(Tr("Touch preference saved. It will apply when Waybridge identifies a window belonging to this app."));
    }

    private Task ApplyLauncherPoliciesAsync()
    {
        launcherPoliciesPending = false;
        if (!controller.Connected || controller.ActiveProfileId != selected.Id || liveSettings is null) return Task.CompletedTask;
        if (busy) { launcherPoliciesPending = true; return Task.CompletedTask; }
        var requested = SettingsWithLauncherPolicies(liveSettings with { TouchModes = selected.Settings.TouchModes });
        static bool SameRules(Dictionary<string, string> previous, Dictionary<string, string> current) =>
            previous.Count == current.Count && current.All(pair => previous.GetValueOrDefault(pair.Key) == pair.Value);
        if (SameRules(liveSettings.TouchModes, requested.TouchModes) &&
            SameRules(liveSettings.AppStreamModes, requested.AppStreamModes))
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
            if (controller.IsRunning) throw new InvalidOperationException(Tr("Wait for the session to reconnect before launching apps"));
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
        var cwdField = new TextBox { PlaceholderText = Tr("Use the app's default directory") };
        var arguments = new TextBox { PlaceholderText = Tr("For example, --profile \"Work\"") };
        var exec = new CheckBox { Content = Tr("Launch via command instead of desktop activation") };
        string[]? extra = null;
        string? cwd = null;
        bool edit = await PromptAsync(Tr("Custom launch · {0}", app.Name), Tr("Preview"),
            Column(Field(L("Additional arguments"), arguments), Field(L("Linux working directory"), cwdField), exec), () =>
            {
                extra = string.IsNullOrWhiteSpace(arguments.Text) ? null : ParseLaunchArguments(arguments.Text);
                cwd = OptionalDirectory(cwdField.Text);
            });
        if (!edit || !await ConnectForLaunchAsync(profile)) return;
        bool directExec = exec.IsChecked == true;
        var preview = await controller.PreviewLaunchAsync(app: app.Id, cwd: cwd, extraArgv: extra,
            launchWithExec: directExec, expectedProfileId: profile.Id);
        string command = preview.TryGetProperty("exec", out var value) ? value.GetString() ?? Tr("Desktop activation") : Tr("Desktop activation");
        string directory = preview.TryGetProperty("cwd", out var path) ? path.GetString() ?? "" : "";
        if (!await ConfirmAsync(Tr("Launch {0}?", app.Name), Tr("{0}\n\nWorking directory · {1}", command, directory), Tr("Launch"))) return;
        await controller.LaunchAsync(app: app.Id, cwd: cwd, extraArgv: extra, launchWithExec: directExec, expectedProfileId: profile.Id);
        await RememberLaunchAsync(profile, app);
    }, canCancelConnect: true);

    private StackPanel BuildCommandPane()
    {
        commandPane.Children.Add(Heading(L("Run a command"), 23));
        commandPane.Children.Add(commandTarget);
        L("Linux working directory").Name(commandDirectory);
        commandPane.Children.Add(Row(L("Working directory"), L("Optional. Enter an absolute path on Linux."), commandDirectory));
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
        if (ActivePage == "apps") ScrollCatalogTo(0);
    }

    private void UpdateCommandControls()
    {
        commandTarget.Text = Tr("Runs in {0}. Quote arguments containing spaces.", selected.Name);
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
        commandHistory.Children.Add(Muted(L("Recent commands · Press Tab to complete the first match")));
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
        if (!text.StartsWith('/')) throw new ArgumentException(Tr("Enter an absolute working directory on Linux"));
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
        if (escaped || quote != '\0') throw new ArgumentException(Tr("The command contains an unfinished quote or escape"));
        if (hasWord) args.Add(current.ToString());
        if (args.Count == 0) throw new ArgumentException(Tr("Enter a command or arguments"));
        return args.ToArray();
    }
}
