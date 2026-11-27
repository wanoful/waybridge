using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private CancellationTokenSource? remoteSessionsLoad;
    private IReadOnlyList<RemoteSessionOverview> remoteSessions = [];
    private bool remoteSessionsChecked, remoteSessionsLoading;
    private string remoteSessionsError = "";
    private DispatcherQueueTimer? remoteSessionRefreshTimer;

    private readonly TextBlock remoteSessionTitle = Heading(L("Remote session"), 17);
    private readonly TextBlock remoteSessionDetail = Muted("");
    private readonly Button refreshRemoteSessionButton = Button(L("Refresh"));
    private readonly Button connectFromRunningButton = Button(L("Connect"), true);
    private readonly Button endSessionRunningButton = Button(L("End remote session"));
    private readonly TextBlock connectionSessionSummary = Muted(L("Disconnecting keeps Linux apps running."));
    private readonly Button refreshRemoteSessionConnectionButton = Button(L("Refresh"));
    private readonly StackPanel processSection = new() { Spacing = 7 };
    private readonly ListView processList = new() { SelectionMode = ListViewSelectionMode.None, MaxHeight = 280 };

    private RemoteSessionOverview? PrimaryRemoteSession =>
        remoteSessions.FirstOrDefault(item => string.Equals(item.Name, selected.Session, StringComparison.Ordinal))
        ?? remoteSessions.FirstOrDefault();

    private bool CanEndRemoteSession =>
        initialized && !busy && (
            (controller.Connected && controller.ActiveProfileId == selected.Id)
            || (paired && !controller.IsRunning && remoteSessions.Count > 0));

    private void InitializeRemoteSessionTimer()
    {
        remoteSessionRefreshTimer = DispatcherQueue.CreateTimer();
        remoteSessionRefreshTimer.Interval = TimeSpan.FromSeconds(10);
        remoteSessionRefreshTimer.IsRepeating = true;
        remoteSessionRefreshTimer.Tick += (_, _) =>
        {
            if (quitting || busy || !initialized || !paired || remoteSessionsLoading) return;
            if (ActivePage is not ("running" or "connection")) return;
            _ = GuardAsync(() => RefreshRemoteSessionsAsync(quiet: true));
        };
        remoteSessionRefreshTimer.Start();
    }

    private UIElement BuildRemoteSessionCard()
    {
        refreshRemoteSessionButton.Click += async (_, _) => await GuardAsync(() => RefreshRemoteSessionsAsync(quiet: false));
        connectFromRunningButton.Click += async (_, _) => await GuardAsync(ConnectOrDisconnectAsync);
        endSessionRunningButton.Click += async (_, _) => await GuardAsync(ConfirmEndAsync);
        refreshRemoteSessionConnectionButton.Click += async (_, _) => await GuardAsync(() => RefreshRemoteSessionsAsync(quiet: false));
        L("Session background processes").Name(processList);
        processSection.Children.Add(Heading(L("Session processes"), 17));
        processSection.Children.Add(processList);
        processSection.Visibility = Visibility.Collapsed;
        return Card(
            Column(
                remoteSessionTitle,
                remoteSessionDetail,
                Actions(refreshRemoteSessionButton, connectFromRunningButton, endSessionRunningButton)),
            new Thickness(15));
    }

    private void InvalidateRemoteSessions()
    {
        var previous = remoteSessionsLoad;
        remoteSessionsLoad = null;
        previous?.Cancel();
        remoteSessions = [];
        remoteSessionsChecked = false;
        remoteSessionsLoading = false;
        remoteSessionsError = "";
        UpdateRemoteSessionView();
    }

    private async Task RefreshRemoteSessionsAsync(bool quiet = true)
    {
        var previous = remoteSessionsLoad;
        remoteSessionsLoad = null;
        previous?.Cancel();

        if (!paired || !profiles.Any(profile => profile.Id == selected.Id))
        {
            remoteSessions = [];
            remoteSessionsChecked = false;
            remoteSessionsLoading = false;
            remoteSessionsError = "";
            UpdateRemoteSessionView();
            return;
        }

        var profile = selected;
        string? credential = secrets.Read(profile.Id);
        if (credential is null)
        {
            remoteSessions = [];
            remoteSessionsChecked = false;
            remoteSessionsLoading = false;
            remoteSessionsError = "";
            UpdateRemoteSessionView();
            return;
        }

        using var request = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        remoteSessionsLoad = request;
        remoteSessionsLoading = true;
        UpdateRemoteSessionView();
        try
        {
            var payload = await new DirectAgent(profile, credential).CallAsync(new { op = "list", details = true }, request.Token);
            if (!ReferenceEquals(remoteSessionsLoad, request) || selected.Id != profile.Id || request.IsCancellationRequested)
                return;
            remoteSessions = RemoteSessionModel.ParseList(payload);
            remoteSessionsChecked = true;
            remoteSessionsError = "";
        }
        catch (OperationCanceledException) when (ReferenceEquals(remoteSessionsLoad, request))
        {
            remoteSessionsChecked = true;
            remoteSessionsError = Tr("Session status request timed out.");
            if (!quiet) Inform(remoteSessionsError);
        }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            if (ReferenceEquals(remoteSessionsLoad, request))
            {
                remoteSessionsChecked = true;
                remoteSessionsError = error.Message;
                if (!quiet) Inform(Tr("Could not check remote session status: ") + error.Message);
            }
        }
        finally
        {
            if (ReferenceEquals(remoteSessionsLoad, request))
            {
                remoteSessionsLoad = null;
                remoteSessionsLoading = false;
                UpdateMode();
            }
        }
    }

    private void UpdateRemoteSessionView()
    {
        bool live = controller.Connected && controller.ActiveProfileId == selected.Id;
        var primary = PrimaryRemoteSession;
        bool hasProfile = profiles.Count > 0;

        refreshRemoteSessionButton.IsEnabled = initialized && hasProfile && paired && !busy && !remoteSessionsLoading;
        refreshRemoteSessionConnectionButton.IsEnabled = refreshRemoteSessionButton.IsEnabled;
        endSessionRunningButton.IsEnabled = CanEndRemoteSession;
        connectFromRunningButton.Visibility = !live && !controller.IsRunning && paired && primary is not null
            ? Visibility.Visible : Visibility.Collapsed;
        connectFromRunningButton.IsEnabled = initialized && !busy;

        if (!hasProfile)
        {
            remoteSessionTitle.Text = Tr("No workstation selected");
            remoteSessionDetail.Text = Tr("Add and pair a Linux workstation to inspect or manage remote sessions.");
            connectionSessionSummary.Text = Tr("Disconnecting keeps Linux apps running.");
        }
        else if (!paired)
        {
            remoteSessionTitle.Text = Tr("Workstation not paired");
            remoteSessionDetail.Text = Tr("Pair this device to check whether a remote session is running.");
            connectionSessionSummary.Text = Tr("Pair this device to inspect or end remote sessions.");
        }
        else if (live)
        {
            string sessionName = controller.CurrentSession ?? primary?.Name ?? selected.Session;
            remoteSessionTitle.Text = Tr("Connected · {0}", sessionName);
            var parts = new List<string>();
            if (primary?.UptimeSeconds is { } uptime) parts.Add(Tr("Running for {0}", RemoteSessionModel.FormatUptime(uptime)));
            if (remoteWindows.Length > 0) parts.Add(Tr("{0} windows", remoteWindows.Length));
            if (backgroundList.Items.Count > 0) parts.Add(Tr("{0} background apps", backgroundList.Items.Count));
            if (primary is { Processes.Count: > 0 }) parts.Add(Tr("{0} processes", primary.Processes.Count));
            remoteSessionDetail.Text = parts.Count > 0
                ? string.Join(" · ", parts) + " · " + Tr("Disconnecting keeps Linux apps running.")
                : Tr("Disconnecting keeps Linux apps running.");
            connectionSessionSummary.Text = remoteSessionTitle.Text + " · " + remoteSessionDetail.Text;
        }
        else if (remoteSessionsLoading && !remoteSessionsChecked)
        {
            remoteSessionTitle.Text = Tr("Checking remote session…");
            remoteSessionDetail.Text = Tr("Querying {0} without connecting.", selected.Name);
            connectionSessionSummary.Text = Tr("Checking remote session status…");
        }
        else if (remoteSessionsError.Length > 0 && primary is null)
        {
            remoteSessionTitle.Text = Tr("Remote session status unavailable");
            remoteSessionDetail.Text = remoteSessionsError;
            connectionSessionSummary.Text = Tr("Could not check remote session status: ") + remoteSessionsError;
        }
        else if (primary is not null)
        {
            remoteSessionTitle.Text = remoteSessions.Count > 1
                ? Tr("Remote sessions running · {0} (+{1})", primary.Name, remoteSessions.Count - 1)
                : Tr("Remote session running · {0}", primary.Name);
            remoteSessionDetail.Text = FormatRemoteSessionSummary(primary);
            connectionSessionSummary.Text = remoteSessionTitle.Text + " · " + remoteSessionDetail.Text;
        }
        else
        {
            remoteSessionTitle.Text = Tr("No remote session running");
            remoteSessionDetail.Text = Tr("No active Waybridge session on {0}. Connect or launch an app to start one.", selected.Name);
            connectionSessionSummary.Text = Tr("No active remote session on this workstation. Disconnecting keeps Linux apps running.");
        }

        if (!live)
            RenderDisconnectedSessionItems(primary);
        RenderSessionProcesses(primary);
        UpdateDockVisibility();
    }

    private static string FormatRemoteSessionSummary(RemoteSessionOverview session)
    {
        var parts = new List<string>();
        if (session.UptimeSeconds is { } uptime) parts.Add(Tr("Running for {0}", RemoteSessionModel.FormatUptime(uptime)));
        parts.Add(Tr("PID {0}", session.Pid));
        if (session.ViewerConnected == true) parts.Add(Tr("Viewer attached"));
        else if (session.ViewerConnected == false) parts.Add(Tr("Running in background (no viewer attached)"));
        if (session.Windows.Count > 0) parts.Add(Tr("{0} windows", session.Windows.Count));
        if (session.BackgroundApps.Count > 0) parts.Add(Tr("{0} background apps", session.BackgroundApps.Count));
        if (session.Processes.Count > 0) parts.Add(Tr("{0} processes", session.Processes.Count));
        return string.Join(" · ", parts);
    }

    private void RenderDisconnectedSessionItems(RemoteSessionOverview? session)
    {
        windowList.Items.Clear();
        backgroundList.Items.Clear();
        backgroundSnapshot = null;

        if (session is not null)
        {
            foreach (var window in session.Windows.Where(item => Matches(item.Title, item.AppId)))
            {
                string title = window.Title.Length > 0 ? window.Title :
                    window.AppId.Length > 0 ? window.AppId : Tr("Window {0}", window.Id);
                string subtitle = window.AppId.Length > 0
                    ? $"{window.AppId} · {(window.X11 ? "X11" : "Wayland")}"
                    : (window.X11 ? "X11" : "Wayland");
                var name = Heading(title, 14);
                name.MaxLines = 1;
                name.TextTrimming = TextTrimming.CharacterEllipsis;
                var row = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    Spacing = 9,
                    Children = { LauncherMark(MatchWindowApplication(window.AppId), 29), Column(name, Muted(subtitle)) }
                };
                windowList.Items.Add(Card(row, new Thickness(12)));
            }

            foreach (var app in session.BackgroundApps.Where(item => Matches(item.Title, item.AppId, item.Id)))
            {
                string title = app.Title.Length > 0 ? app.Title : (app.AppId.Length > 0 ? app.AppId : app.Id);
                var caption = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    Spacing = 8,
                    Children = { LauncherMark(MatchWindowApplication(app.AppId), 28), Heading(title, 14) }
                };
                var stateLabel = app.Status == "NeedsAttention"
                    ? Muted(L("App needs attention"))
                    : Muted(L("Running in background"));
                backgroundList.Items.Add(Card(Column(caption, stateLabel), new Thickness(12)));
            }
        }

        windowSection.Visibility = windowList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        backgroundSection.Visibility = backgroundList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdateMatchingWindows();
    }

    private void RenderSessionProcesses(RemoteSessionOverview? session)
    {
        processList.Items.Clear();
        if (session is not null)
        {
            foreach (var proc in session.Processes)
            {
                var matchedApp = proc.AppId.Length > 0 ? MatchWindowApplication(proc.AppId) : null;
                string label = matchedApp?.Name ?? proc.Name;
                if (!Matches(label, proc.Name, proc.AppId)) continue;
                string detail = proc.Count > 1
                    ? Tr("PID {0} · {1} processes", proc.Pid, proc.Count)
                    : Tr("PID {0}", proc.Pid);
                if (matchedApp is not null && !string.Equals(matchedApp.Name, proc.Name, StringComparison.OrdinalIgnoreCase))
                    detail = $"{proc.Name} · {detail}";
                var titleBlock = Heading(label, 14);
                titleBlock.MaxLines = 1;
                titleBlock.TextTrimming = TextTrimming.CharacterEllipsis;
                var content = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                    Spacing = 9,
                    Children = { LauncherMark(matchedApp, 26), Column(titleBlock, Muted(detail)) }
                };
                processList.Items.Add(Card(content, new Thickness(11)));
            }
        }
        processSection.Visibility = processList.Items.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private Task EndDisconnectedSessionAsync(ConnectionProfile profile, RemoteSessionOverview target) => RunOperationAsync(async () =>
    {
        string credential = secrets.Read(profile.Id) ?? throw new InvalidOperationException(Tr("Pair this device again"));
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var agent = new DirectAgent(profile, credential);
        await agent.CallAsync(new
        {
            op = "end",
            session = target.Name,
            expected_pid = target.Pid,
            expected_started = target.Started
        }, timeout.Token);

        NotifyTerminalsDisconnected(Tr("The remote session ended."));

        // Wait briefly for the compositor to remove its descriptor after SIGTERM.
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(4);
        IReadOnlyList<RemoteSessionOverview> latest = remoteSessions;
        while (DateTimeOffset.UtcNow < deadline)
        {
            await Task.Delay(200, timeout.Token);
            try
            {
                var payload = await agent.CallAsync(new { op = "list", details = true }, timeout.Token);
                latest = RemoteSessionModel.ParseList(payload);
                if (!latest.Any(s => s.Name == target.Name && s.Pid == target.Pid && s.Started == target.Started))
                    break;
            }
            catch { break; }
        }
        if (selected.Id == profile.Id)
        {
            remoteSessions = latest;
            remoteSessionsChecked = true;
            remoteSessionsError = "";
            UpdateRemoteSessionView();
        }
        Inform(Tr("Remote session “{0}” ended.", target.Name));
    });
}
