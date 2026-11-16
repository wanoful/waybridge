using System.IO.Pipes;
using System.Buffers.Binary;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Authentication;
using System.Text;
using System.Text.Json;
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
    var unpaired = new ConnectionProfile { Mode = "direct", Host = bootstrap.GetProperty("host").GetString()!,
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
    await using (var controller = new SessionController(settingsDirectory,appHost,_ => throw new Exception("direct connection unexpectedly requested SSH trust")))
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
        Check((await controller.ApplicationsAsync()).Any(app => app.Id == "waybridge-direct-smoke.desktop"),"direct controller application catalog failed");
        string controllerMarker = Path.Combine(directRuntime,"controller-marker");
        await controller.LaunchAsync(argv:["python3","-I","-c","import pathlib,sys;pathlib.Path(sys.argv[1]).write_text(sys.argv[2])",controllerMarker,"controller 你好"]);
        for (int i=0;i<100 && !File.Exists(controllerMarker);i++) await Task.Delay(25,deadline.Token);
        Check(File.ReadAllText(controllerMarker)=="controller 你好","controller direct launch failed");
        await controller.SendEngineAsync(new {type="crash"});
        await restored.Task.WaitAsync(TimeSpan.FromSeconds(15));
        Check(bootstraps >= 2 && controller.Connected,"direct controller did not repeat private transport bootstrap on reconnect");
        Check((await direct.CallAsync(new {op="info",session=sessionName},deadline.Token)).GetProperty("pid").GetInt32() == pid,"engine restart replaced remote session");
        await controller.DisconnectAsync();
        Check(!controller.IsRunning && !controller.Connected,"direct controller did not disconnect");
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
if (args is ["--ssh-smoke", var sshPort, var identityFile, var serverBinary, var runtime])
{
    using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(60));
    var sshProfile = new ConnectionProfile { Host = "127.0.0.1", User = Environment.UserName,
        SshPort = int.Parse(sshPort), IdentityFile = identityFile, ServerBinary = serverBinary };
    string settingsDirectory = Path.Combine(runtime, "control center 你好");
    int prompts = 0;
    await Reject<OperationCanceledException>(async () => await SshAgent.StartAsync(sshProfile, settingsDirectory,
        _ => Task.FromResult(false), deadline.Token));
    Check(!File.Exists(Path.Combine(settingsDirectory, "known_hosts")), "rejected host persisted");
    int pid;
    await using (var agent = await SshAgent.StartAsync(sshProfile, settingsDirectory, request =>
    {
        prompts++;
        Check(request.Fingerprints.Any(f => f.StartsWith("ssh-ed25519 SHA256:", StringComparison.Ordinal)), "missing host fingerprint");
        return Task.FromResult(true);
    }, deadline.Token))
    {
        var session = await agent.CallAsync(new { op = "ensure", session = "gui-ssh-test", binary = serverBinary,
            port = 0, codec = "png", audio = false, xwayland = false }, deadline.Token);
        pid = session.GetProperty("pid").GetInt32();
        await using (var tunnel = await agent.TunnelAsync(sshProfile, settingsDirectory, session.GetProperty("port").GetInt32(), deadline.Token))
        {
            using var viewer = new TcpClient();
            await viewer.ConnectAsync("127.0.0.1", tunnel.Port, deadline.Token);
            byte[] hello = Encoding.UTF8.GetBytes("\0" + JsonSerializer.Serialize(new { type = "hello", version = 3, token = session.GetProperty("token").GetString() }));
            byte[] header = new byte[4]; BinaryPrimitives.WriteInt32BigEndian(header, hello.Length);
            await viewer.GetStream().WriteAsync(header, deadline.Token);
            await viewer.GetStream().WriteAsync(hello, deadline.Token);
            await viewer.GetStream().ReadExactlyAsync(header, deadline.Token);
            int size = BinaryPrimitives.ReadInt32BigEndian(header);
            Check(size is > 1 and < 65536, "invalid tunneled handshake");
            byte[] response = new byte[size]; await viewer.GetStream().ReadExactlyAsync(response, deadline.Token);
            Check(response[0] == 0 && JsonDocument.Parse(response.AsMemory(1)).RootElement.GetProperty("type").GetString() == "hello", "tunneled handshake failed");
        }
        await agent.CallAsync(new { op = "launch", session = "gui-ssh-test", binary = serverBinary,
            argv = new[] { "python3", "-c", "import pathlib,sys;pathlib.Path(sys.argv[1]).write_text(sys.argv[2])", Path.Combine(runtime,"ssh-marker"), "你好 ; $(literal)" } }, deadline.Token);
        for (int i = 0; i < 100 && !File.Exists(Path.Combine(runtime,"ssh-marker")); i++) await Task.Delay(25, deadline.Token);
        Check(File.ReadAllText(Path.Combine(runtime,"ssh-marker")) == "你好 ; $(literal)", "SSH literal launch failed");
    }
    await using (var agent = await SshAgent.StartAsync(sshProfile, settingsDirectory, _ => throw new Exception("known host prompted again"), deadline.Token))
    {
        var attached = await agent.CallAsync(new { op = "ensure", session = "gui-ssh-test", mode = "attach" }, deadline.Token);
        Check(attached.GetProperty("pid").GetInt32() == pid && !attached.GetProperty("created").GetBoolean(), "SSH disconnect killed remote session");
        await agent.CallAsync(new { op = "end", session = "gui-ssh-test" }, deadline.Token);
    }
    Check(prompts == 1, "unexpected trust prompt count");
    Console.WriteLine("PASS: real OpenSSH rejection/TOFU/strict reuse, embedded Python RPC, authenticated forwarding, Unicode literal launch, retained session and explicit shutdown");
    return;
}
string directory = Path.Combine(Path.GetTempPath(), "waybridge-core-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(directory);
try
{
    var profile = new ConnectionProfile { Host = "linux.example", User = "tester", ServerBinary = "/opt/My App/waybridge's binary" };
    profile.Validate();
    foreach (var host in new[] { "-oProxyCommand=evil", "x;touch /tmp/evil", "u@host", "x\nbad" })
        await Reject<ArgumentException>(() => { (profile with { Host = host }).Validate(); return Task.CompletedTask; });
    foreach (string session in new[] { "../other", "/tmp/session", "x;rm", "bad\nname" })
        await Reject<ArgumentException>(() => { (profile with { Session = session }).Validate(); return Task.CompletedTask; });
    await Reject<ArgumentException>(() => { new ClientSettings { Fps = 10000 }.Validate(); return Task.CompletedTask; });
    Check(new ClientSettings { Codec = "auto", Hdr = true }.ServerCodec == "auto", "GUI must preserve auto/HDR policy for the shared server resolver");
    var directProfile = profile with { Mode = "direct", Port = 49092, CertificateSha256 = new string('a', 64), ClientId = "paired-client" };
    directProfile.Validate();
    await Reject<ArgumentException>(() => { (directProfile with { CertificateSha256 = "bad-pin" }).Validate(); return Task.CompletedTask; });
    await Reject<ArgumentException>(() => { (directProfile with { Port = 0 }).Validate(); return Task.CompletedTask; });
    await Reject<ArgumentException>(() => { _ = new DirectAgent(directProfile with { CertificateSha256 = "" }, new string('b',64)); return Task.CompletedTask; });
    Console.WriteLine("PASS: profile validation rejects option, shell and session-path injection");

    var store = new ProfileStore(directory);
    await store.SaveAsync([profile]);
    var loaded = await store.LoadAsync();
    Check(loaded.Single() == profile, "profile round trip");
    Check(!File.ReadAllText(Path.Combine(directory, "profiles.json")).Contains("token", StringComparison.OrdinalIgnoreCase), "credential field in profile");
    await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => store.SaveAsync([profile])));
    Check((await store.LoadAsync()).Count == 1, "atomic concurrent save");
    Console.WriteLine("PASS: atomic profile persistence without credential fields");

    using var bytes = new MemoryStream(Encoding.UTF8.GetBytes("{\"value\":\"中文😀\"}\n{\"id\":2}\n"));
    var lines = new JsonLines(bytes, 1024);
    Check((await lines.ReadAsync(default))!.Value.GetProperty("value").GetString() == "中文😀", "unicode framing");
    Check((await lines.ReadAsync(default))!.Value.GetProperty("id").GetInt32() == 2, "coalesced framing");
    Check(await lines.ReadAsync(default) is null, "EOF");
    await Reject<IOException>(async () => { using var huge = new MemoryStream(new byte[1024]); await new JsonLines(huge, 1024).ReadAsync(default); });
    await Reject<IOException>(async () => { using var partial = new MemoryStream(Encoding.UTF8.GetBytes("{\"a\":1}")); await new JsonLines(partial).ReadAsync(default); });
    Console.WriteLine("PASS: UTF-8/coalesced JSON framing rejects oversized and incomplete messages");

    var ssh = SshAgent.Arguments(profile, Path.Combine(directory, "host keys"));
    Check(ssh.Contains("BatchMode=yes") && ssh.Contains("StrictHostKeyChecking=yes"), "SSH trust disabled");
    Check(ssh[^1] == "tester@linux.example", "SSH destination changed");
    string quoted = SshAgent.PosixQuote("a'b\nc");
    Check(quoted == "'a'\"'\"'b\nc'", "remote quoting");
    Console.WriteLine("PASS: SSH argv preserves host verification and literal remote command quoting");

    string bootstrap = (string)typeof(SshAgent).GetField("BootstrapCommand", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
    Check(bootstrap.Length < 1024 && !bootstrap.Contains("import configparser", StringComparison.Ordinal), "SSH loader grew into Windows argv payload");
    using (var embedded = typeof(SshAgent).Assembly.GetManifestResourceStream("Waybridge.SessionControl")!)
    using (var source = new StreamReader(embedded))
    {
        string management = Convert.ToBase64String(Encoding.UTF8.GetBytes(await source.ReadToEndAsync()));
        Check(!bootstrap.Contains(management, StringComparison.Ordinal), "management payload leaked into argv");
        Check(management.Length <= 1024 * 1024, "management script exceeds bounded bootstrap frame");
    }
    Console.WriteLine("PASS: constant-size SSH loader keeps management script outside Windows command line");

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

    string appHost = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location)!, "Waybridge.Control.Tests" + (OperatingSystem.IsWindows() ? ".exe" : ""));
    await using (var controller = new SessionController(directory, appHost, _ => Task.FromResult(false)))
    {
        int connections = 0;
        var restored = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        controller.ConnectionChanged += value => { if (value && Interlocked.Increment(ref connections) == 2) restored.TrySetResult(); };
        await controller.ConnectAsync(new ConnectionProfile { Host = "127.0.0.1", Mode = "tunnel", AutoReconnect = true }, "test-secret-" + new string('b', 40));
        Check(controller.Connected && controller.IsRunning, "controller failed to connect");
        await controller.SendEngineAsync(new { type = "crash" });
        await restored.Task.WaitAsync(TimeSpan.FromSeconds(12));
        Check(controller.Connected, "controller failed to reconnect");
        await controller.DisconnectAsync();
        Check(!controller.IsRunning && !controller.Connected, "controller failed to stop");
        Check(controller.Logs.All(line => !line.Contains("test-secret-")), "secret leaked to diagnostic log");
    }
    Console.WriteLine("PASS: controller reconnects after engine death and explicitly disconnects without leaking credentials");
}
finally { Directory.Delete(directory, true); }
