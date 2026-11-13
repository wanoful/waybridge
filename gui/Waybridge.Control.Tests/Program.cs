using System.IO.Pipes;
using System.Buffers.Binary;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Waybridge.Control.Core;

static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
static async Task Reject<T>(Func<Task> action) where T : Exception
{
    try { await action(); } catch (T) { return; }
    throw new Exception("Expected " + typeof(T).Name);
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
    await pipe.WriteAsync(Wire.Encode(new { type = "connected", version = 3 }));
    while (await input.ReadAsync(default) is { } request)
    {
        if (request.GetProperty("type").GetString() == "disconnect") break;
        if (request.GetProperty("type").GetString() == "crash") Environment.Exit(3);
        await pipe.WriteAsync(Wire.Encode(new { type = "observed", request }));
    }
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
