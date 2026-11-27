using System.Diagnostics;
using System.Text.Json;
using Waybridge.Control.Core;

namespace Waybridge.Cli;

internal static class Program
{
    public static async Task<int> Main(string[] args)
    {
        string defaultState = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Waybridge");
        InitializeLanguage(defaultState);

        if (args.Length == 0 || args[0] is "-h" or "--help" or "help")
        {
            PrintUsage();
            return 0;
        }

        try
        {
            return args[0] switch
            {
                "shell" => await RunShellAsync(args[1..], defaultState),
                "list" => await RunListAsync(args[1..], defaultState),
                "status" => await RunStatusAsync(args[1..], defaultState),
                "end" => await RunEndAsync(args[1..], defaultState),
                _ => UnknownCommand(args[0])
            };
        }
        catch (OperationCanceledException)
        {
            return 130;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"[Waybridge] {error.Message}");
            return 1;
        }
    }

    private static int UnknownCommand(string command)
    {
        Console.Error.WriteLine(Tr("Unknown command: {0}", command));
        PrintUsage();
        return 2;
    }

    private static void PrintUsage()
    {
        Console.WriteLine("Usage:");
        Console.WriteLine("  waybridge shell --connection <name-or-id> [--cwd <linux-dir>]");
        Console.WriteLine("  waybridge status --connection <name-or-id> [--json]");
        Console.WriteLine("  waybridge end --connection <name-or-id> [--session <name>]");
        Console.WriteLine("  waybridge list");
    }

    private static void InitializeLanguage(string stateDirectory)
    {
        string preference = "system";
        try
        {
            string path = Path.Combine(stateDirectory, "launcher.json");
            if (File.Exists(path))
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(path));
                if (doc.RootElement.TryGetProperty("language", out var value) && value.ValueKind == JsonValueKind.String)
                    preference = value.GetString() ?? "system";
            }
        }
        catch { }
        Localization.Initialize(preference);
    }

    private static async Task<int> RunListAsync(string[] args, string defaultState)
    {
        string stateDir = defaultState;
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--state-dir" && i + 1 < args.Length) stateDir = args[++i];
            else if (args[i] is "-h" or "--help") { PrintUsage(); return 0; }
            else throw new ArgumentException(Tr("Unexpected argument: {0}", args[i]));
        }
        var profiles = await new ProfileStore(stateDir).LoadAsync();
        if (profiles.Count == 0)
        {
            Console.WriteLine(Tr("No saved Waybridge connections."));
            return 0;
        }
        foreach (var profile in profiles)
        {
            string paired = profile.ClientId.Length > 0 && profile.CertificateSha256.Length == 64 ? "paired" : "unpaired";
            Console.WriteLine($"{profile.Name}\t{profile.Id:D}\t{profile.Host}:{profile.Port}\t{paired}");
        }
        return 0;
    }

    private static async Task<(ConnectionProfile Profile, string Token)> LoadAuthorizedProfileAsync(string stateDir, string? connectionQuery)
    {
        if (string.IsNullOrWhiteSpace(connectionQuery))
            throw new ArgumentException(Tr("Specify a workstation name or connection ID with --connection."));
        var profiles = await new ProfileStore(stateDir).LoadAsync();
        var profile = ProfileStore.FindConnection(profiles, connectionQuery);
        if (profile.ClientId.Length == 0 || profile.CertificateSha256.Length != 64)
            throw new InvalidOperationException(Tr("Pair with “{0}” in Waybridge Control Center first.", profile.Name));
        string? token = Environment.GetEnvironmentVariable("WAYBRIDGE_CLIENT_TOKEN") ?? ReadWindowsSecret(profile.Id);
        if (string.IsNullOrWhiteSpace(token))
            throw new InvalidOperationException(Tr("Saved credentials for “{0}” were not found. Pair again in Waybridge Control Center.", profile.Name));
        return (profile, token);
    }

    private static async Task<int> RunStatusAsync(string[] args, string defaultState)
    {
        string? connectionQuery = null;
        string stateDir = defaultState;
        bool jsonOutput = false;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--connection" or "-c":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --connection."));
                    connectionQuery = args[++i];
                    break;
                case "--state-dir":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --state-dir."));
                    stateDir = args[++i];
                    break;
                case "--json":
                    jsonOutput = true;
                    break;
                case "-h" or "--help":
                    PrintUsage();
                    return 0;
                default:
                    throw new ArgumentException(Tr("Unexpected argument: {0}", args[i]));
            }
        }
        var (profile, token) = await LoadAuthorizedProfileAsync(stateDir, connectionQuery);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var payload = await new DirectAgent(profile, token).CallAsync(new { op = "list", details = true }, timeout.Token);
        if (jsonOutput)
        {
            Console.WriteLine(JsonSerializer.Serialize(payload, new JsonSerializerOptions { WriteIndented = true }));
            return 0;
        }
        var sessions = RemoteSessionModel.ParseList(payload);
        if (sessions.Count == 0)
        {
            Console.WriteLine(Tr("No remote session is running on “{0}”.", profile.Name));
            return 0;
        }
        foreach (var session in sessions)
        {
            string viewer = session.ViewerConnected switch
            {
                true => Tr("Viewer attached"),
                false => Tr("Running in background (no viewer attached)"),
                _ => ""
            };
            string uptime = session.UptimeSeconds is { } sec ? RemoteSessionModel.FormatUptime(sec) : "";
            var summary = new[] { $"PID {session.Pid}", uptime, viewer }.Where(s => s.Length > 0);
            Console.WriteLine($"{session.Name}\t{string.Join(" · ", summary)}");
            foreach (var win in session.Windows)
                Console.WriteLine($"  [window] {(win.Title.Length > 0 ? win.Title : win.AppId)} ({(win.X11 ? "X11" : "Wayland")})");
            foreach (var bg in session.BackgroundApps)
                Console.WriteLine($"  [background] {(bg.Title.Length > 0 ? bg.Title : bg.AppId)} ({bg.Status})");
            foreach (var proc in session.Processes)
                Console.WriteLine(proc.Count > 1
                    ? $"  [process] {proc.Name} (PID {proc.Pid}, x{proc.Count})"
                    : $"  [process] {proc.Name} (PID {proc.Pid})");
        }
        return 0;
    }

    private static async Task<int> RunEndAsync(string[] args, string defaultState)
    {
        string? connectionQuery = null;
        string? sessionName = null;
        string stateDir = defaultState;
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--connection" or "-c":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --connection."));
                    connectionQuery = args[++i];
                    break;
                case "--session":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --session."));
                    sessionName = args[++i];
                    break;
                case "--state-dir":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --state-dir."));
                    stateDir = args[++i];
                    break;
                case "-h" or "--help":
                    PrintUsage();
                    return 0;
                default:
                    throw new ArgumentException(Tr("Unexpected argument: {0}", args[i]));
            }
        }
        var (profile, token) = await LoadAuthorizedProfileAsync(stateDir, connectionQuery);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var agent = new DirectAgent(profile, token);
        var sessions = RemoteSessionModel.ParseList(await agent.CallAsync(new { op = "list" }, timeout.Token));
        if (sessions.Count == 0)
        {
            Console.WriteLine(Tr("No remote session is running on “{0}”.", profile.Name));
            return 0;
        }
        RemoteSessionOverview? target = !string.IsNullOrWhiteSpace(sessionName)
            ? sessions.FirstOrDefault(s => string.Equals(s.Name, sessionName, StringComparison.Ordinal))
            : sessions.FirstOrDefault(s => string.Equals(s.Name, profile.Session, StringComparison.Ordinal)) ?? sessions[0];
        if (target is null)
            throw new KeyNotFoundException(Tr("Remote session “{0}” was not found on “{1}”.", sessionName!, profile.Name));
        await agent.CallAsync(new { op = "end", session = target.Name, expected_pid = target.Pid, expected_started = target.Started }, timeout.Token);
        Console.WriteLine(Tr("Remote session “{0}” ended.", target.Name));
        return 0;
    }

    private static async Task<int> RunShellAsync(string[] args, string defaultState)
    {
        string? connectionQuery = null;
        string? cwd = null;
        string? sessionOverride = null;
        string stateDir = defaultState;
        bool noConnect = false;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--connection" or "-c":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --connection."));
                    connectionQuery = args[++i];
                    break;
                case "--cwd":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --cwd."));
                    cwd = args[++i];
                    break;
                case "--session":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --session."));
                    sessionOverride = args[++i];
                    break;
                case "--state-dir":
                    if (i + 1 >= args.Length) throw new ArgumentException(Tr("Missing value for --state-dir."));
                    stateDir = args[++i];
                    break;
                case "--no-connect":
                    noConnect = true;
                    break;
                case "-h" or "--help":
                    PrintUsage();
                    return 0;
                default:
                    throw new ArgumentException(Tr("Unexpected argument: {0}", args[i]));
            }
        }

        if (string.IsNullOrWhiteSpace(connectionQuery))
            throw new ArgumentException(Tr("Specify a workstation name or connection ID with --connection."));

        var profiles = await new ProfileStore(stateDir).LoadAsync();
        var profile = ProfileStore.FindConnection(profiles, connectionQuery);

        if (profile.ClientId.Length == 0 || profile.CertificateSha256.Length != 64)
            throw new InvalidOperationException(Tr("Pair with “{0}” in Waybridge Control Center first.", profile.Name));

        string? token = noConnect
            ? Environment.GetEnvironmentVariable("WAYBRIDGE_CLIENT_TOKEN") ?? ReadWindowsSecret(profile.Id)
            : ReadWindowsSecret(profile.Id);
        if (string.IsNullOrWhiteSpace(token))
            throw new InvalidOperationException(Tr("Saved credentials for “{0}” were not found. Pair again in Waybridge Control Center.", profile.Name));

        using var cts = new CancellationTokenSource();
        ControlIpcClient? ipc = null;
        try
        {
            string session;
            if (noConnect)
            {
                session = string.IsNullOrWhiteSpace(sessionOverride) ? profile.Session : sessionOverride;
            }
            else
            {
                ipc = await EnsureGraphicalConnectionAsync(stateDir, profile, cts.Token);
                session = string.IsNullOrWhiteSpace(sessionOverride) ? ipc.Session : sessionOverride;
            }

            using var terminal = ConsoleTerminal.Enter(() => cts.Cancel());
            var (initialCols, initialRows) = terminal.GetSize();
            await using var shell = await ShellConnection.OpenAsync(
                profile, token, session, initialCols, initialRows, "xterm-256color", cwd, cts.Token);

            int exitCode = 0;
            string? disconnectNotice = null;
            using var sessionCts = CancellationTokenSource.CreateLinkedTokenSource(cts.Token);

            Task<string>? ipcWatch = ipc?.WaitForDisconnectAsync(sessionCts.Token);

            var readRemote = Task.Run(async () =>
            {
                try
                {
                    while (!sessionCts.IsCancellationRequested)
                    {
                        var frame = await shell.ReadFrameAsync(sessionCts.Token);
                        if (frame is null)
                        {
                            disconnectNotice ??= Tr("Remote shell connection closed.");
                            break;
                        }
                        switch (frame.Value.Kind)
                        {
                            case ShellProtocol.KindData:
                                terminal.WriteOutput(frame.Value.Payload);
                                break;
                            case ShellProtocol.KindExit:
                                var (code, reason) = ShellProtocol.ParseExit(frame.Value.Payload);
                                exitCode = code;
                                disconnectNotice = reason switch
                                {
                                    ShellProtocol.ReasonSessionEnded => Tr("Remote session ended."),
                                    ShellProtocol.ReasonRevoked => Tr("This device was unpaired from the Linux service."),
                                    _ => null
                                };
                                return;
                            case ShellProtocol.KindPing:
                            case ShellProtocol.KindPong:
                                break;
                        }
                    }
                }
                catch (OperationCanceledException) when (sessionCts.IsCancellationRequested) { }
                catch (IOException) when (sessionCts.IsCancellationRequested) { }
                catch (IOException)
                {
                    disconnectNotice ??= Tr("Remote shell connection closed.");
                    exitCode = 1;
                }
                finally { sessionCts.Cancel(); }
            });

            var writeInput = Task.Run(async () =>
            {
                byte[] buffer = new byte[4096];
                try
                {
                    while (!sessionCts.IsCancellationRequested)
                    {
                        int count = await terminal.ReadInputAsync(buffer, sessionCts.Token);
                        if (count <= 0) break;
                        await shell.WriteDataAsync(buffer.AsMemory(0, count), sessionCts.Token);
                    }
                }
                catch (OperationCanceledException) { }
                catch (IOException) { }
            });

            var watchResize = Task.Run(async () =>
            {
                var (lastCols, lastRows) = (initialCols, initialRows);
                try
                {
                    while (!sessionCts.IsCancellationRequested)
                    {
                        await Task.Delay(100, sessionCts.Token);
                        var (cols, rows) = terminal.GetSize();
                        if (cols != lastCols || rows != lastRows)
                        {
                            lastCols = cols;
                            lastRows = rows;
                            await shell.WriteResizeAsync(cols, rows, sessionCts.Token);
                        }
                    }
                }
                catch (OperationCanceledException) { }
                catch (IOException) { }
            });

            var keepalive = Task.Run(async () =>
            {
                try
                {
                    while (!sessionCts.IsCancellationRequested)
                    {
                        await Task.Delay(TimeSpan.FromSeconds(20), sessionCts.Token);
                        await shell.WritePingAsync(sessionCts.Token);
                    }
                }
                catch (OperationCanceledException) { }
                catch (IOException) { }
            });

            if (ipcWatch is not null)
            {
                var finished = await Task.WhenAny(readRemote, ipcWatch);
                if (ReferenceEquals(finished, ipcWatch) && !sessionCts.IsCancellationRequested)
                {
                    disconnectNotice = await ipcWatch;
                    exitCode = 0;
                    sessionCts.Cancel();
                }
            }
            await readRemote;
            sessionCts.Cancel();
            try { await Task.WhenAll(writeInput, watchResize, keepalive); } catch { }

            terminal.Dispose();
            if (!string.IsNullOrWhiteSpace(disconnectNotice))
                Console.Error.WriteLine($"\n[Waybridge] {disconnectNotice}");
            return exitCode;
        }
        finally
        {
            if (ipc is not null) await ipc.DisposeAsync();
        }
    }

    private static string? ReadWindowsSecret(Guid profileId) =>
        OperatingSystem.IsWindows() ? new WindowsCredentialStore().Read(profileId) : null;

    private static async Task<ControlIpcClient> EnsureGraphicalConnectionAsync(
        string stateDir,
        ConnectionProfile profile,
        CancellationToken cancellationToken)
    {
        string pipeName = ControlIpc.PipeName(stateDir);
        try
        {
            return await ConnectWithProgressAsync(pipeName, profile, TimeSpan.FromMilliseconds(350), cancellationToken);
        }
        catch (TimeoutException) { }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { }

        string controlExe = Path.Combine(AppContext.BaseDirectory, "Waybridge.Control.exe");
        if (!File.Exists(controlExe))
            throw new FileNotFoundException(Tr("Waybridge.Control.exe was not found next to waybridge.exe."), controlExe);

        Console.Error.WriteLine(Tr("Starting Waybridge Control Center…"));
        var info = new ProcessStartInfo(controlExe) { UseShellExecute = false };
        info.ArgumentList.Add("--background");
        Process.Start(info)?.Dispose();

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                return await ConnectWithProgressAsync(pipeName, profile, TimeSpan.FromMilliseconds(500), cancellationToken);
            }
            catch (TimeoutException) { }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { }
            await Task.Delay(200, cancellationToken);
        }
        throw new TimeoutException(Tr("Timed out waiting for Waybridge Control Center to start."));
    }

    private static async Task<ControlIpcClient> ConnectWithProgressAsync(
        string pipeName,
        ConnectionProfile profile,
        TimeSpan pipeConnectTimeout,
        CancellationToken cancellationToken)
    {
        using var slowNotice = new CancellationTokenSource();
        var noticeTask = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(400, slowNotice.Token);
                Console.Error.WriteLine(Tr("Connecting to “{0}”…", profile.Name));
            }
            catch (OperationCanceledException) { }
        });
        try
        {
            return await ControlIpcClient.ConnectAndEnsureAsync(pipeName, profile.Id, pipeConnectTimeout, cancellationToken);
        }
        finally
        {
            slowNotice.Cancel();
            await noticeTask;
        }
    }
}
