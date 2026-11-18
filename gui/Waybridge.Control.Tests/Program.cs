using System.IO.Pipes;
using System.Reflection;
using System.Security.Authentication;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using Waybridge.Control.Core;

static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
static async Task Reject<T>(Func<Task> action) where T : Exception
{
    try { await action(); } catch (T) { return; }
    throw new Exception("Expected " + typeof(T).Name);
}

if (args is ["--silent-engine", var silentMarkerArgument, "--control-pipe", var silentPipe])
{
    using var pipe = new NamedPipeClientStream(".", silentPipe, PipeDirection.InOut, PipeOptions.Asynchronous);
    await pipe.ConnectAsync(5000);
    var start = await new JsonLines(pipe).ReadAsync(default);
    Check(start?.GetProperty("type").GetString() == "start", "missing silent-worker bootstrap");
    await File.WriteAllTextAsync(silentMarkerArgument, Environment.ProcessId.ToString());
    // Reproduce an alive worker that never reports ready or closes stderr.
    await Task.Delay(Timeout.Infinite);
    return;
}

if (args is ["--control-pipe", var pipeName])
{
    using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
    await pipe.ConnectAsync(5000);
    var input = new JsonLines(pipe);
    var start = (await input.ReadAsync(default))!.Value;
    Check(start.GetProperty("type").GetString() == "start", "missing start");
    string secret = start.GetProperty("token").GetString()!;
    Check(secret.Length >= 16 && !Environment.GetCommandLineArgs().Any(a => a.Contains(secret, StringComparison.Ordinal)), "secret leaked to argv");
    if (start.TryGetProperty("transport", out var transport) && transport.ValueKind == JsonValueKind.Object)
    {
        string clientSecret = transport.GetProperty("client_token").GetString()!;
        Check(clientSecret.Length >= 32, "missing direct client credential");
        Check(!Environment.GetCommandLineArgs().Any(argument => argument.Contains(clientSecret, StringComparison.Ordinal)), "direct credential leaked to argv");
        Check(!Environment.GetEnvironmentVariables().Values.Cast<string>().Any(value => value.Contains(clientSecret, StringComparison.Ordinal) || value.Contains(secret, StringComparison.Ordinal)), "credential leaked to environment");
        Check(transport.GetProperty("type").GetString() == "tls", "direct transport type changed");
        string fingerprint = transport.GetProperty("fingerprint").GetString()!;
        Check(fingerprint.Length == 64 && fingerprint.All(Uri.IsHexDigit), "direct transport omitted certificate pin");
        Check(!string.IsNullOrWhiteSpace(transport.GetProperty("client_id").GetString()), "direct transport omitted client identity");
        await pipe.WriteAsync(Wire.Encode(new { type = "bootstrap_verified", transport_type = "tls", session = transport.GetProperty("session").GetString() }));
    }
    // This process is the IPC boundary: report the actual serialized settings,
    // without echoing credentials, before acknowledging startup.
    await pipe.WriteAsync(Wire.Encode(new { type = "bootstrap_settings", settings = start.GetProperty("settings") }));
    await pipe.WriteAsync(Wire.Encode(new { type = "connected", version = 3 }));
    while (await input.ReadAsync(default) is { } request)
    {
        if (request.GetProperty("type").GetString() == "disconnect") break;
        if (request.GetProperty("type").GetString() == "crash") Environment.Exit(3);
        await pipe.WriteAsync(Wire.Encode(new { type = "observed", request }));
    }
    return;
}
if (args is ["--direct-smoke"])
{
    using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(120));
    var bootstrap = (await new JsonLines(Console.OpenStandardInput(), 65536).ReadAsync(deadline.Token))
        ?? throw new IOException("missing private direct-test bootstrap");
    string pairingCode = bootstrap.GetProperty("pair_code").GetString()!;
    string directRuntime = bootstrap.GetProperty("runtime").GetString()!;
    string sessionName = bootstrap.GetProperty("session").GetString()!;
    var unpaired = new ConnectionProfile { Host = bootstrap.GetProperty("host").GetString()!,
        Port = bootstrap.GetProperty("port").GetInt32(), CertificateSha256 = bootstrap.GetProperty("certificate_sha256").GetString()!,
        Session = sessionName, Settings = new ClientSettings {Codec = "png", Audio = false} };
    Check(!Environment.GetCommandLineArgs().Any(argument => argument.Contains(pairingCode, StringComparison.Ordinal)), "pair code leaked to argv");
    Check(!Environment.GetEnvironmentVariables().Values.Cast<string>().Any(value => value.Contains(pairingCode, StringComparison.Ordinal)), "pair code leaked to environment");
    string changedPin = (unpaired.CertificateSha256[0] == '0' ? "1" : "0") + unpaired.CertificateSha256[1..];
    await Reject<AuthenticationException>(() => DirectAgent.PairAsync(unpaired with {CertificateSha256 = changedPin}, pairingCode, "test", deadline.Token));
    await Reject<AuthenticationException>(() => DirectAgent.PairAsync(unpaired, new string('x', 43), "test", deadline.Token));
    var credentials = await DirectAgent.PairAsync(unpaired, pairingCode, "Windows 测试设备", deadline.Token);
    await Reject<AuthenticationException>(() => DirectAgent.PairAsync(unpaired, pairingCode, "replay", deadline.Token));
    var directProfile = unpaired with {ClientId = credentials.ClientId};
    directProfile.Validate();
    var direct = new DirectAgent(directProfile, credentials.ClientToken);
    await Reject<AuthenticationException>(() => new DirectAgent(directProfile, new string('x', 43)).CallAsync(new {op="list"}, deadline.Token));
    var initialSessions = await direct.CallAsync(new {op="list"}, deadline.Token);
    Check(initialSessions.ValueKind == JsonValueKind.Array && initialSessions.GetArrayLength() == 0, "private direct service did not start empty");
    Console.WriteLine("PASS: .NET pinned TLS rejects changed identity, invalid/replayed pairing codes and incorrect credentials");

    var actualSession = await direct.CallAsync(new {op="ensure",session=sessionName,mode="auto",codec="png",audio=false,xwayland=false}, deadline.Token);
    int pid = actualSession.GetProperty("pid").GetInt32();
    string sessionSecret = actualSession.GetProperty("token").GetString()!;
    Check(actualSession.GetProperty("created").GetBoolean(), "actual session was not created");
    var listed = await direct.CallAsync(new {op="list"}, deadline.Token);
    Check(listed.EnumerateArray().Any(item => item.GetProperty("pid").GetInt32() == pid), "created session missing from list");
    Check(listed.EnumerateArray().All(item => !item.TryGetProperty("token", out _)), "list leaked session credentials");
    var information = await direct.CallAsync(new {op="info",session=sessionName}, deadline.Token);
    Check(information.GetProperty("pid").GetInt32() == pid && !information.TryGetProperty("token",out _), "invalid session info");
    var directApplications = await direct.CallAsync(new {op="applications"}, deadline.Token);
    Check(directApplications.ValueKind == JsonValueKind.Array && directApplications.EnumerateArray().Any(item => item.GetProperty("id").GetString() == "waybridge-direct-smoke.desktop"), "private application fixture missing from direct catalog");
    string marker = Path.Combine(directRuntime,"direct-marker");
    await direct.CallAsync(new {op="launch",session=sessionName,argv=new[]{"python3","-I","-c","import pathlib,sys;pathlib.Path(sys.argv[1]).write_text(sys.argv[2])",marker,"你好 ; $(literal)"}}, deadline.Token);
    for (int i=0;i<100 && !File.Exists(marker);i++) await Task.Delay(25, deadline.Token);
    Check(File.ReadAllText(marker) == "你好 ; $(literal)", "direct literal Unicode application launch failed");
    Console.WriteLine("PASS: .NET direct RPC creates real compositor, lists sessions/applications and launches Unicode argv without shell evaluation");

    string settingsDirectory = Path.Combine(directRuntime,"direct settings 你好");
    var profileStore = new ProfileStore(settingsDirectory);
    await profileStore.SaveAsync([directProfile]);
    string stored = File.ReadAllText(Path.Combine(settingsDirectory,"profiles.json"));
    Check(!stored.Contains(credentials.ClientToken,StringComparison.Ordinal) && !stored.Contains(sessionSecret,StringComparison.Ordinal), "direct credential persisted in profile JSON");
    var firstTransport = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
    var restored = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
    int connections = 0, bootstraps = 0;
    string appHost = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location)!, "Waybridge.Control.Tests" + (OperatingSystem.IsWindows() ? ".exe" : ""));
    await using (var controller = new SessionController(appHost))
    {
        controller.Message += message => {
            if (message.GetProperty("type").GetString() == "bootstrap_verified")
            {
                Check(message.GetProperty("transport_type").GetString() == "tls" && message.GetProperty("session").GetString() == sessionName, "native transport bootstrap changed");
                Interlocked.Increment(ref bootstraps); firstTransport.TrySetResult();
            }
        };
        controller.ConnectionChanged += value => {if (value && Interlocked.Increment(ref connections) == 2) restored.TrySetResult();};
        await controller.ConnectAsync(directProfile with {SessionMode="attach"},credentials.ClientToken);
        await firstTransport.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Check(controller.Connected && controller.CurrentSession == sessionName,"direct controller did not attach");
        var launcherApp = (await controller.ApplicationsAsync(directProfile.Id)).Single(app => app.Id == "waybridge-direct-smoke.desktop");
        Check(launcherApp.GenericName == "Direct editor" && launcherApp.Comment == "Desktop metadata fixture" &&
            launcherApp.Categories.SequenceEqual(["Development"]) && launcherApp.Keywords.SequenceEqual(["launcher"]) &&
            launcherApp.StartupWmClass == "WaybridgeDirect" &&
            launcherApp.Actions.SequenceEqual([new RemoteDesktopAction("NewWindow", "Open a window", "Open a new editor")]),
            "direct controller application catalog metadata failed");
        var desktopPreview = await controller.PreviewLaunchAsync(app: launcherApp.Id, action: "NewWindow", cwd: directRuntime,
            extraArgv: ["中文文字"], expectedProfileId: directProfile.Id, cancellationToken: deadline.Token);
        Check(desktopPreview.GetProperty("action").GetString() == "NewWindow" &&
            desktopPreview.GetProperty("cwd").GetString() == directRuntime &&
            desktopPreview.GetProperty("extra_argv")[0].GetString() == "中文文字", "direct Desktop Action preview failed");
        await controller.LaunchAsync(app: launcherApp.Id, action: "NewWindow", expectedProfileId: directProfile.Id,
            cancellationToken: deadline.Token);
        string controllerMarker = Path.Combine(directRuntime,"controller-marker");
        string[] command = ["python3", "-I", "-c", "import pathlib,sys;pathlib.Path(sys.argv[1]).write_text(sys.argv[2])",
            controllerMarker, "controller 你好"];
        await Reject<InvalidOperationException>(() => controller.LaunchAsync(argv: command, expectedProfileId: Guid.NewGuid()));
        await Reject<OperationCanceledException>(() => controller.LaunchAsync(argv: command, expectedProfileId: directProfile.Id,
            cancellationToken: new CancellationToken(true)));
        Check(!File.Exists(controllerMarker), "cancelled or stale host launch reached the session");
        var commandPreview = await controller.PreviewLaunchAsync(argv: command, cwd: directRuntime,
            expectedProfileId: directProfile.Id, cancellationToken: deadline.Token);
        Check(!File.Exists(controllerMarker) && commandPreview.GetProperty("cwd").GetString() == directRuntime,
            "literal command preview executed or lost working directory");
        await controller.LaunchAsync(argv: command, cwd: directRuntime, expectedProfileId: directProfile.Id, cancellationToken: deadline.Token);
        for (int i=0;i<100 && !File.Exists(controllerMarker);i++) await Task.Delay(25,deadline.Token);
        Check(File.ReadAllText(controllerMarker)=="controller 你好","controller direct launch failed");
        await controller.SendEngineAsync(new {type="crash"});
        await restored.Task.WaitAsync(TimeSpan.FromSeconds(15));
        Check(bootstraps >= 2 && controller.Connected,"direct controller did not repeat private transport bootstrap on reconnect");
        Check((await direct.CallAsync(new {op="info",session=sessionName},deadline.Token)).GetProperty("pid").GetInt32() == pid,"engine restart replaced remote session");
        await controller.DisconnectAsync();
        Check(!controller.IsRunning && !controller.Connected && controller.ActiveProfileId is null,
            "direct controller did not disconnect or clear the previous host");
        var attached = await direct.CallAsync(new {op="ensure",session=sessionName,mode="attach"},deadline.Token);
        Check(!attached.GetProperty("created").GetBoolean() && attached.GetProperty("pid").GetInt32() == pid,"viewer disconnect killed direct session");
        Check(controller.Logs.All(line => !line.Contains(credentials.ClientToken,StringComparison.Ordinal) && !line.Contains(sessionSecret,StringComparison.Ordinal)),"direct credentials leaked to diagnostics");
        await controller.ConnectAsync(directProfile with {SessionMode="attach"},credentials.ClientToken);
        await controller.EndSessionAsync();
        Check(!controller.IsRunning,"explicit direct session end did not stop controller");
    }
    for(int i=0;i<100;i++)
    {
        var sessions=await direct.CallAsync(new{op="list"},deadline.Token);
        if(sessions.GetArrayLength()==0) break;
        await Task.Delay(25,deadline.Token);
    }
    Check((await direct.CallAsync(new{op="list"},deadline.Token)).GetArrayLength()==0,"explicit direct end did not remove session");
    var clients=await direct.CallAsync(new{op="clients"},deadline.Token);
    Check(clients.EnumerateArray().Any(item=>item.GetProperty("client_id").GetString()==credentials.ClientId),"paired client missing from trust list");
    var revoked=await direct.CallAsync(new{op="revoke",revoke_client_id=credentials.ClientId},deadline.Token);
    Check(revoked.GetProperty("revoked").GetBoolean(),"direct client was not revoked");
    await Reject<AuthenticationException>(()=>direct.CallAsync(new{op="list"},deadline.Token));
    Console.WriteLine("PASS: direct controller privately bootstraps native transport, reconnects retained session, ends explicitly, keeps credentials out of profiles/logs and enforces revocation");
    return;
}
string directory = Path.Combine(Path.GetTempPath(), "waybridge-core-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(directory);
try
{
    var profile = new ConnectionProfile { Host = "linux.example" };
    profile.Validate();
    Check(profile.Port == 49092, "new profiles must default to the Waybridge TLS endpoint");
    foreach (string host in new[] { "::1", "[::1]", "2001:db8::1" }) (profile with { Host = host }).Validate();
    foreach (var host in new[] { "--not-a-host", "host;other", "u@host", "x\nbad", "host\n", "[[::1]]", "127.0.0.1:49092" })
        await Reject<ArgumentException>(() => { (profile with { Host = host }).Validate(); return Task.CompletedTask; });
    foreach (string session in new[] { "../other", "/tmp/session", "x;rm", "bad\nname", "session\n" })
        await Reject<ArgumentException>(() => { (profile with { Session = session }).Validate(); return Task.CompletedTask; });
    await Reject<ArgumentException>(() => { new ClientSettings { Fps = 10000 }.Validate(); return Task.CompletedTask; });
    Check(new ClientSettings { Codec = "auto", Hdr = true }.ServerCodec == "auto", "GUI must preserve auto/HDR policy for the shared server resolver");
    var directProfile = profile with { CertificateSha256 = new string('a', 64), ClientId = "paired-client" };
    directProfile.Validate();
    await Reject<ArgumentException>(() => { (directProfile with { CertificateSha256 = "bad-pin" }).Validate(); return Task.CompletedTask; });
    await Reject<ArgumentException>(() => { (directProfile with { Port = 0 }).Validate(); return Task.CompletedTask; });
    await Reject<ArgumentException>(() => { _ = new DirectAgent(directProfile with { CertificateSha256 = "" }, new string('b',64)); return Task.CompletedTask; });
    Console.WriteLine("PASS: TLS profile validates host, port, pin and session name");

    var defaultSettings = new ClientSettings { Files = false };
    Check(!defaultSettings.Files && defaultSettings.DragText, "file sharing defaults must not disable text drags");
    Check(!defaultSettings.Touch && defaultSettings.TouchModes.Count == 0, "touch must default to mouse-compatible mode");
    Check(!defaultSettings.Pen && defaultSettings.PenPressureGamma == 1 && !defaultSettings.Gamepads &&
        defaultSettings.GamepadRumble && defaultSettings.GamepadDeadzone == 15, "new profiles must use independent opt-in and calibration defaults");
    foreach (double invalid in new[] { .249, 4.001, double.NaN, double.PositiveInfinity, double.NegativeInfinity })
        await Reject<ArgumentException>(() => { new ClientSettings { PenPressureGamma = invalid }.Validate(); return Task.CompletedTask; });
    foreach (int invalid in new[] { -1, 51 })
        await Reject<ArgumentException>(() => { new ClientSettings { GamepadDeadzone = invalid }.Validate(); return Task.CompletedTask; });
    new ClientSettings { PenPressureGamma = .25, GamepadDeadzone = 0 }.Validate();
    new ClientSettings { PenPressureGamma = 4, GamepadDeadzone = 50 }.Validate();
    var textDisabled = JsonSerializer.Deserialize<ClientSettings>(JsonSerializer.Serialize(new ClientSettings { DragText = false }, ProfileStore.Json), ProfileStore.Json)!;
    Check(!textDisabled.DragText && textDisabled.Files, "independent text drag policy failed to persist");
    var store = new ProfileStore(directory);
    await store.SaveAsync([profile]);
    var loaded = await store.LoadAsync();
    Check(JsonSerializer.Serialize(loaded.Single(), ProfileStore.Json) ==
        JsonSerializer.Serialize(profile, ProfileStore.Json), "profile round trip");
    Check(!File.ReadAllText(Path.Combine(directory, "profiles.json")).Contains("token", StringComparison.OrdinalIgnoreCase), "credential field in profile");
    await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => store.SaveAsync([profile])));
    Check((await store.LoadAsync()).Count == 1, "atomic concurrent save");
    Console.WriteLine("PASS: atomic profile persistence without credential fields");

    var touchProfile = profile with { Settings = new ClientSettings {
        Touch = true, Ime = false, ReadOnly = true, Files = false, DragText = false, Clipboard = "off",
        TouchModes = new() { ["org.example.Drawing"] = "native", ["org.example.Editor"] = "mouse" } } };
    await store.SaveAsync([touchProfile]);
    var touchLoaded = (await new ProfileStore(directory).LoadAsync()).Single();
    Check(touchLoaded.Settings.Touch && !touchLoaded.Settings.Ime && touchLoaded.Settings.ReadOnly &&
        !touchLoaded.Settings.Files && !touchLoaded.Settings.DragText && touchLoaded.Settings.Clipboard == "off",
        "native touch preference did not persist independently of IME/read-only/file policy");
    Check(touchLoaded.Settings.TouchModes["org.example.Editor"] == "mouse" &&
        touchLoaded.Settings.TouchModes["org.example.Drawing"] == "native", "per-application touch modes did not persist");
    await Reject<ArgumentException>(() => Task.Run(() => (new ClientSettings { TouchModes = new() { ["app"] = "guess" } }).Validate()));
    await store.SaveAsync([touchLoaded with { Settings = touchLoaded.Settings with { Touch = false } }]);
    var touchDisabled = (await new ProfileStore(directory).LoadAsync()).Single();
    Check(!touchDisabled.Settings.Touch && !touchDisabled.Settings.Ime && touchDisabled.Settings.ReadOnly &&
        !touchDisabled.Settings.Files && !touchDisabled.Settings.DragText,
        "disabling saved native touch changed an unrelated input/sharing policy");
    using (var saved = JsonDocument.Parse(await File.ReadAllTextAsync(Path.Combine(directory, "profiles.json"))))
        Check(saved.RootElement[0].GetProperty("settings").GetProperty("touch").ValueKind == JsonValueKind.False,
            "disabled touch was not persisted as an explicit false setting");
    var professional = touchProfile with { Settings = touchProfile.Settings with {
        Pen = true, PenPressureGamma = 1.75, Gamepads = true, GamepadRumble = false, GamepadDeadzone = 27 } };
    await store.SaveAsync([professional]);
    var professionalLoaded = (await new ProfileStore(directory).LoadAsync()).Single();
    Check(professionalLoaded.Settings.Pen && professionalLoaded.Settings.PenPressureGamma == 1.75 &&
        professionalLoaded.Settings.Gamepads && !professionalLoaded.Settings.GamepadRumble && professionalLoaded.Settings.GamepadDeadzone == 27,
        "professional input settings did not persist through disk");
    Check(professionalLoaded.Settings.Touch && !professionalLoaded.Settings.Ime && professionalLoaded.Settings.ReadOnly &&
        !professionalLoaded.Settings.Files && professionalLoaded.Settings.TouchModes["org.example.Editor"] == "mouse",
        "professional input settings changed existing independent policies");
    await store.SaveAsync([professionalLoaded with { Settings = professionalLoaded.Settings with { Pen = false } }]);
    var penDisabled = (await store.LoadAsync()).Single();
    Check(!penDisabled.Settings.Pen && penDisabled.Settings.Gamepads && penDisabled.Settings.PenPressureGamma == 1.75,
        "disabling pen changed gamepad or pressure calibration");
    await store.SaveAsync([penDisabled with { Settings = penDisabled.Settings with { Gamepads = false, GamepadRumble = true } }]);
    var professionalDisabled = (await store.LoadAsync()).Single();
    Check(!professionalDisabled.Settings.Pen && !professionalDisabled.Settings.Gamepads && professionalDisabled.Settings.GamepadRumble &&
        professionalDisabled.Settings.GamepadDeadzone == 27 && professionalDisabled.Settings.ReadOnly,
        "disabling gamepad lost saved calibration or input policies");
    Console.WriteLine("PASS: independent touch, pen and gamepad policies and bounded calibration persist");
    using var bytes = new MemoryStream(Encoding.UTF8.GetBytes("{\"value\":\"中文😀\"}\n{\"id\":2}\n"));
    var lines = new JsonLines(bytes, 1024);
    Check((await lines.ReadAsync(default))!.Value.GetProperty("value").GetString() == "中文😀", "unicode framing");
    Check((await lines.ReadAsync(default))!.Value.GetProperty("id").GetInt32() == 2, "coalesced framing");
    Check(await lines.ReadAsync(default) is null, "EOF");
    await Reject<IOException>(async () => { using var huge = new MemoryStream(new byte[1024]); await new JsonLines(huge, 1024).ReadAsync(default); });
    await Reject<IOException>(async () => { using var partial = new MemoryStream(Encoding.UTF8.GetBytes("{\"a\":1}")); await new JsonLines(partial).ReadAsync(default); });
    Console.WriteLine("PASS: UTF-8/coalesced JSON framing rejects oversized and incomplete messages");

    var observed = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
    await using (var engine = await EngineChannel.StartAsync("dotnet", "127.0.0.1", 49090, "test-secret-" + new string('a', 40),
        new(), message => { if (message.GetProperty("type").GetString() == "observed") observed.TrySetResult(message); },
        _ => { }, default, [Assembly.GetExecutingAssembly().Location]))
    {
        await engine.SendAsync(new { type = "settings", settings = new ClientSettings { ReadOnly = true, Clipboard = "off" } });
        var reply = await observed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Check(reply.GetProperty("request").GetProperty("settings").GetProperty("read_only").GetBoolean(), "IPC settings");
        Check(reply.GetProperty("request").GetProperty("settings").GetProperty("clipboard").GetString() == "off", "IPC clipboard");
    }
    Console.WriteLine("PASS: separate-process named-pipe bootstrap, private credential transport, commands and graceful disconnect");

    foreach (bool initialTouch in new[] { false, true })
    {
        var settings = new ClientSettings { Touch = initialTouch, Ime = false, ReadOnly = true, Files = false };
        var channelEvents = Channel.CreateUnbounded<JsonElement>();
        await using var touchEngine = await EngineChannel.StartAsync("dotnet", "127.0.0.1", 49090,
            "test-touch-" + new string('d', 40), settings,
            message => channelEvents.Writer.TryWrite(message), _ => { }, default,
            [Assembly.GetExecutingAssembly().Location]);
        var bootstrapSettings = await channelEvents.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        Check(bootstrapSettings.GetProperty("type").GetString() == "bootstrap_settings", "missing native touch bootstrap observation");
        var actual = bootstrapSettings.GetProperty("settings");
        Check(actual.GetProperty("touch").GetBoolean() == initialTouch && !actual.GetProperty("ime").GetBoolean() &&
            actual.GetProperty("read_only").GetBoolean() && !actual.GetProperty("files").GetBoolean(),
            "EngineChannel start lost native touch or unrelated policy fields");
        foreach (bool liveTouch in new[] { !initialTouch, initialTouch })
        {
            await touchEngine.SendAsync(new { type = "settings", settings = settings with { Touch = liveTouch } });
            JsonElement response;
            do { response = await channelEvents.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5)); }
            while (response.GetProperty("type").GetString() != "observed");
            var live = response.GetProperty("request").GetProperty("settings");
            Check(live.GetProperty("touch").GetBoolean() == liveTouch && !live.GetProperty("ime").GetBoolean() &&
                live.GetProperty("read_only").GetBoolean() && !live.GetProperty("files").GetBoolean(),
                "EngineChannel live native touch toggle changed or lost independent policies");
        }
    }
    Console.WriteLine("PASS: separate-process native touch startup and live on/off settings retain explicit booleans and independent policies");
    foreach (bool initial in new[] { false, true })
    {
        var settings = new ClientSettings { Pen = initial, PenPressureGamma = .75, Gamepads = !initial,
            GamepadRumble = false, GamepadDeadzone = 23, Touch = true, Ime = false, ReadOnly = true, Files = false };
        var messages = Channel.CreateUnbounded<JsonElement>();
        await using var engine = await EngineChannel.StartAsync("dotnet", "127.0.0.1", 49090,
            "test-input-" + new string('e', 40), settings, value => messages.Writer.TryWrite(value), _ => { },
            default, [Assembly.GetExecutingAssembly().Location]);
        var startup = await messages.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        Check(startup.GetProperty("type").GetString() == "bootstrap_settings", "missing professional input startup observation");
        var serialized = startup.GetProperty("settings");
        Check(serialized.GetProperty("pen").GetBoolean() == initial && serialized.GetProperty("pen_pressure_gamma").GetDouble() == .75 &&
            serialized.GetProperty("gamepads").GetBoolean() == !initial && !serialized.GetProperty("gamepad_rumble").GetBoolean() &&
            serialized.GetProperty("gamepad_deadzone").GetInt32() == 23, "professional startup settings did not cross process boundary");
        foreach (var liveSettings in new[] { settings with { Pen = !initial, PenPressureGamma = 2 },
                     settings with { Gamepads = initial, GamepadRumble = true, GamepadDeadzone = 0 } })
        {
            await engine.SendAsync(new { type = "settings", settings = liveSettings });
            JsonElement response;
            do { response = await messages.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5)); }
            while (response.GetProperty("type").GetString() != "observed");
            var actualSettings = response.GetProperty("request").GetProperty("settings");
            Check(actualSettings.GetProperty("pen").GetBoolean() == liveSettings.Pen &&
                actualSettings.GetProperty("pen_pressure_gamma").GetDouble() == liveSettings.PenPressureGamma &&
                actualSettings.GetProperty("gamepads").GetBoolean() == liveSettings.Gamepads &&
                actualSettings.GetProperty("gamepad_rumble").GetBoolean() == liveSettings.GamepadRumble &&
                actualSettings.GetProperty("gamepad_deadzone").GetInt32() == liveSettings.GamepadDeadzone,
                "professional live settings lost independent feature toggles or calibration");
            Check(actualSettings.GetProperty("touch").GetBoolean() && !actualSettings.GetProperty("ime").GetBoolean() &&
                actualSettings.GetProperty("read_only").GetBoolean() && !actualSettings.GetProperty("files").GetBoolean(),
                "professional live settings altered unrelated input policies");
        }
    }
    Console.WriteLine("PASS: separate-process pen/gamepad startup and independent live toggles carry pressure, deadzone and rumble without changing touch/read-only/IME/files");

    string silentMarker = Path.Combine(directory, "silent-worker.pid");
    using (var cancellation = new CancellationTokenSource())
    {
        var startup = EngineChannel.StartAsync("dotnet", "127.0.0.1", 49090, "test-secret-" + new string('c', 40),
            new(), _ => { }, _ => { }, cancellation.Token,
            [Assembly.GetExecutingAssembly().Location, "--silent-engine", silentMarker]);
        int silentProcessId = 0;
        for (int i = 0; i < 250; i++)
        {
            try { if (File.Exists(silentMarker) && int.TryParse(File.ReadAllText(silentMarker), out silentProcessId)) break; }
            catch (IOException) { }
            await Task.Delay(20);
        }
        Check(silentProcessId > 0, "silent worker never received bootstrap");
        using var silentWorker = System.Diagnostics.Process.GetProcessById(silentProcessId);
        cancellation.Cancel();
        try
        {
            await Reject<OperationCanceledException>(async () =>
            {
                await using var unexpected = await startup.WaitAsync(TimeSpan.FromSeconds(8));
            });
            Check(silentWorker.HasExited, "cancelled startup left its worker alive");
        }
        finally
        {
            if (!silentWorker.HasExited) silentWorker.Kill(true);
            try { await startup.WaitAsync(TimeSpan.FromSeconds(4)); } catch (OperationCanceledException) { }
        }
    }
    Console.WriteLine("PASS: cancellation of a silent connected worker is bounded and terminates it without waiting for live stderr EOF");

}
finally { Directory.Delete(directory, true); }
