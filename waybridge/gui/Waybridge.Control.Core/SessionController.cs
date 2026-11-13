using System.Text.Json;

namespace Waybridge.Control.Core;

public sealed class SessionController(string directory, string enginePath, Func<TrustRequest, Task<bool>> trust) : IAsyncDisposable
{
    private CancellationTokenSource? operation;
    private Task? loop;
    private SshAgent? agent;
    private SshTunnel? tunnel;
    private EngineChannel? engine;
    private ConnectionProfile? active;
    private string? currentSession;
    private string currentToken = "";
    private volatile bool running;
    public bool Connected { get; private set; }
    public bool IsRunning => running;
    public string? CurrentSession => currentSession;
    public Guid? ActiveProfileId => active?.Id;
    public event Action<string>? Status;
    public event Action<string>? Log;
    public event Action<JsonElement>? Message;
    public event Action<bool>? ConnectionChanged;
    private readonly Queue<string> diagnostics = new();
    public string[] Logs { get { lock (diagnostics) return diagnostics.ToArray(); } }
    private void WriteLog(string text)
    {
        if (currentToken.Length > 0) text = text.Replace(currentToken, "[令牌已隐藏]", StringComparison.Ordinal);
        if (text.Length > 2048) text = text[..2048];
        lock (diagnostics) { diagnostics.Enqueue(text); while (diagnostics.Count > 200) diagnostics.Dequeue(); }
        Log?.Invoke(text);
    }
    public async Task ConnectAsync(ConnectionProfile profile, string manualToken = "")
    {
        profile.Validate();
        await DisconnectAsync();
        active = profile with { Settings = profile.Settings with { } };
        running = true;
        operation = new CancellationTokenSource();
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        loop = RunAsync(active, manualToken, ready, operation.Token);
        await ready.Task;
    }
    private async Task RunAsync(ConnectionProfile profile, string manualToken, TaskCompletionSource ready, CancellationToken token)
    {
        bool hadConnection = false; int attempt = 0;
        try
        {
            while (!token.IsCancellationRequested)
            {
                try
                {
                    string host; int port;
                    if (profile.Mode == "ssh")
                    {
                        Status?.Invoke(attempt == 0 ? "正在建立 SSH 连接…" : $"正在重新连接（第 {attempt} 次）…");
                        agent = await SshAgent.StartAsync(profile, directory, trust, token);
                        Status?.Invoke("正在检查或启动远端会话…");
                        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
                        deadline.CancelAfter(TimeSpan.FromSeconds(40));
                        var result = await agent.CallAsync(new
                        {
                            op = "ensure", session = currentSession ?? profile.Session,
                            mode = currentSession is null ? profile.SessionMode : "attach", binary = profile.ServerBinary,
                            port = profile.Port, fps = profile.Settings.Fps, bitrate = profile.Settings.Bitrate,
                            codec = profile.Settings.ServerCodec, udp = profile.Settings.Udp, hdr = profile.Settings.Hdr, gpu_input = profile.Settings.GpuInput
                        }, deadline.Token);
                        currentSession = result.GetProperty("name").GetString()!;
                        currentToken = result.GetProperty("token").GetString()!;
                        int remotePort = result.GetProperty("port").GetInt32();
                        WriteLog($"会话 {currentSession} 已就绪；{(result.GetProperty("created").GetBoolean() ? "新建" : "接入")}，远端端口 {remotePort}");
                        Status?.Invoke("正在建立串流隧道…");
                        tunnel = await agent.TunnelAsync(profile, directory, remotePort, token);
                        host = "127.0.0.1"; port = tunnel.Port;
                        if (profile.Settings.Udp && string.IsNullOrWhiteSpace(profile.Settings.UdpHost)) profile.Settings.UdpHost = profile.Host;
                    }
                    else { host = profile.Host; port = profile.Port; currentToken = manualToken; }
                    Status?.Invoke("正在连接串流内核…");
                    engine = await EngineChannel.StartAsync(enginePath, host, port, currentToken, profile.Settings,
                        message => Message?.Invoke(message), WriteLog, token);
                    Connected = true; ConnectionChanged?.Invoke(true); Status?.Invoke("已连接");
                    hadConnection = true; attempt = 0; ready.TrySetResult();
                    Task cancelled = Task.Delay(Timeout.Infinite, token);
                    var waits = new List<Task> { engine.Exited, engine.ControlClosed, cancelled };
                    if (agent is not null) waits.Add(agent.Exited);
                    if (tunnel is not null) waits.Add(tunnel.Exited);
                    await Task.WhenAny(waits);
                    token.ThrowIfCancellationRequested();
                    throw new IOException("连接已断开");
                }
                catch (OperationCanceledException) when (token.IsCancellationRequested) { break; }
                catch (Exception error)
                {
                    WriteLog(error.Message);
                    if (!hadConnection || !profile.AutoReconnect)
                    {
                        ready.TrySetException(error); Status?.Invoke(error.Message); break;
                    }
                    attempt++;
                    Status?.Invoke("连接中断，正在等待重连…");
                }
                finally { await ReleaseConnectionAsync(); }
                await Task.Delay(TimeSpan.FromSeconds(Math.Min(15, Math.Pow(2, Math.Min(attempt, 4)))), token);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { ready.TrySetCanceled(token); }
        finally { ready.TrySetCanceled(token); await ReleaseConnectionAsync(); running = false; ConnectionChanged?.Invoke(false); }
    }
    private async Task ReleaseConnectionAsync()
    {
        Connected = false; ConnectionChanged?.Invoke(false);
        if (engine is { } e) { engine = null; await e.DisposeAsync(); }
        if (tunnel is { } t) { tunnel = null; await t.DisposeAsync(); }
        if (agent is { } a) { agent = null; await a.DisposeAsync(); }
        currentToken = "";
    }
    public async Task DisconnectAsync()
    {
        if (operation is not null)
        {
            operation.Cancel();
            if (loop is not null) try { await loop; } catch (OperationCanceledException) { }
            operation.Dispose(); operation = null; loop = null;
        }
        currentSession = null;
        Status?.Invoke("未连接");
    }
    public async Task ApplyAsync(ClientSettings settings)
    {
        settings.Validate();
        if (engine is null) throw new InvalidOperationException("请先连接");
        await engine.SendAsync(new { type = "settings", settings });
        if (active is not null) active.Settings = settings with { };
    }
    public async Task<IReadOnlyList<RemoteApplication>> ApplicationsAsync()
    {
        if (agent is null) return [];
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var result = await agent.CallAsync(new { op = "applications" }, timeout.Token);
        return result.Deserialize<List<RemoteApplication>>(Wire.Json) ?? [];
    }
    public async Task LaunchAsync(string? app = null, string[]? argv = null)
    {
        if (agent is null || active is null || currentSession is null) throw new InvalidOperationException("应用启动需要内置 SSH 会话");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await agent.CallAsync(new { op = "launch", session = currentSession, binary = active.ServerBinary, app, argv }, timeout.Token);
    }
    public async Task EndSessionAsync()
    {
        if (agent is null || currentSession is null) throw new InvalidOperationException("结束会话需要内置 SSH");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        // Cancel reconnect before the expected remote shutdown.
        if (active is not null) active.AutoReconnect = false;
        await agent.CallAsync(new { op = "end", session = currentSession }, timeout.Token);
        await DisconnectAsync();
    }
    public Task SendEngineAsync(object request) => engine?.SendAsync(request) ?? throw new InvalidOperationException("请先连接");
    public async ValueTask DisposeAsync() => await DisconnectAsync();
}
