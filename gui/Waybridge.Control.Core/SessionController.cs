using System.Security.Authentication;
using System.Text.Json;

namespace Waybridge.Control.Core;

public sealed class SessionController(string enginePath) : IAsyncDisposable
{
    private sealed record Connection(ConnectionProfile Profile, DirectAgent Server, EngineChannel Worker,
        string Session, CancellationToken Closed);

    private readonly SemaphoreSlim lifecycle = new(1, 1);
    private readonly Queue<string> diagnostics = new();
    private CancellationTokenSource? operation;
    private Task? loop;
    private volatile ConnectionProfile? active;
    private volatile Connection? connection;
    private volatile string? currentSession;

    public bool Connected => connection is not null;
    public bool IsRunning => active is not null;
    public string? CurrentSession => currentSession;
    public Guid? ActiveProfileId => active?.Id;
    public string[] Logs { get { lock (diagnostics) return diagnostics.ToArray(); } }
    public event Action<string>? Status;
    public event Action<string>? Log;
    public event Action<JsonElement>? Message;
    public event Action<bool>? ConnectionChanged;

    private void WriteLog(string text)
    {
        if (text.Length > 2048) text = text[..2048];
        lock (diagnostics) { diagnostics.Enqueue(text); while (diagnostics.Count > 200) diagnostics.Dequeue(); }
        Log?.Invoke(text);
    }

    public async Task ConnectAsync(ConnectionProfile profile, string clientToken, CancellationToken cancellationToken = default)
    {
        profile.Validate();
        var requested = profile with { Settings = CopySettings(profile.Settings) };
        var server = new DirectAgent(requested, clientToken);
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        CancellationTokenSource started;
        await lifecycle.WaitAsync(cancellationToken);
        try
        {
            await StopAsync();
            cancellationToken.ThrowIfCancellationRequested();
            active = requested;
            started = operation = new();
            loop = RunAsync(requested, server, clientToken, ready, started.Token);
        }
        finally { lifecycle.Release(); }

        try { await ready.Task.WaitAsync(cancellationToken); }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await lifecycle.WaitAsync();
            try { if (ReferenceEquals(operation, started)) await StopAsync(); }
            finally { lifecycle.Release(); }
            throw;
        }
    }

    private async Task RunAsync(ConnectionProfile profile, DirectAgent server, string clientToken,
        TaskCompletionSource ready, CancellationToken token)
    {
        var hadConnection = false;
        var attempt = 0;
        var sessionToken = "";
        Exception? failure = null;
        try
        {
            while (true)
            {
                token.ThrowIfCancellationRequested();
                try
                {
                    Status?.Invoke(attempt == 0 ? "正在验证 Linux 服务并接入会话…" : $"正在重新连接（第 {attempt} 次）…");
                    var settings = profile.Settings;
                    var result = await server.CallAsync(new
                    {
                        op = "ensure", session = currentSession ?? profile.Session,
                        mode = currentSession is null ? profile.SessionMode : "attach",
                        port = 0, fps = settings.Fps, bitrate = settings.Bitrate,
                        codec = settings.ServerCodec, udp = settings.Udp, hdr = settings.Hdr, gpu_input = settings.GpuInput
                    }, token);
                    var session = result.GetProperty("name").GetString() ?? throw new IOException("Linux 服务未返回会话名称");
                    sessionToken = result.GetProperty("token").GetString() ?? throw new IOException("Linux 服务未返回会话令牌");
                    currentSession = session;
                    WriteLog($"加密服务已验证，会话 {session} 已就绪");
                    Status?.Invoke("正在连接串流内核…");
                    if (settings.Udp && string.IsNullOrWhiteSpace(settings.UdpHost))
                        settings = settings with { UdpHost = profile.Address };

                    await using var worker = await EngineChannel.StartAsync(enginePath, profile.Address, profile.Port,
                        sessionToken, settings, message => Message?.Invoke(message), WriteLog, token,
                        transport: server.Transport(session), sourceId: profile.Id.ToString("N"));
                    using var lifetime = CancellationTokenSource.CreateLinkedTokenSource(token);
                    token.ThrowIfCancellationRequested();
                    connection = new(profile, server, worker, session, lifetime.Token);
                    try
                    {
                        hadConnection = true;
                        attempt = 0;
                        ConnectionChanged?.Invoke(true);
                        Status?.Invoke("已连接");
                        ready.TrySetResult();
                        await Task.WhenAny(worker.Exited, worker.ControlClosed, Task.Delay(Timeout.Infinite, lifetime.Token));
                        token.ThrowIfCancellationRequested();
                        throw new IOException("连接已断开");
                    }
                    finally
                    {
                        lifetime.Cancel();
                        connection = null;
                        ConnectionChanged?.Invoke(false);
                    }
                }
                catch (OperationCanceledException) when (token.IsCancellationRequested) { throw; }
                catch (Exception error)
                {
                    string safeMessage = error.Message.Replace(clientToken, "[客户端凭据已隐藏]", StringComparison.Ordinal);
                    if (sessionToken.Length > 0)
                        safeMessage = safeMessage.Replace(sessionToken, "[令牌已隐藏]", StringComparison.Ordinal);
                    WriteLog(safeMessage);
                    if (!hadConnection || !profile.AutoReconnect || error is AuthenticationException or ArgumentException)
                    {
                        failure = safeMessage == error.Message ? error : new IOException(safeMessage);
                        Status?.Invoke(safeMessage);
                        break;
                    }
                    attempt++;
                    Status?.Invoke("连接中断，正在等待重连…");
                }
                await Task.Delay(TimeSpan.FromSeconds(Math.Min(15, Math.Pow(2, Math.Min(attempt, 4)))), token);
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        finally
        {
            currentSession = null;
            active = null;
            if (failure is not null) ready.TrySetException(failure);
            else ready.TrySetCanceled(token);
            ConnectionChanged?.Invoke(false);
        }
    }

    private async Task StopAsync()
    {
        if (operation is null) return;
        operation.Cancel();
        try { if (loop is not null) await loop; }
        finally
        {
            operation.Dispose();
            operation = null;
            loop = null;
        }
    }

    public async Task DisconnectAsync()
    {
        await lifecycle.WaitAsync();
        try
        {
            await StopAsync();
            Status?.Invoke("未连接");
        }
        finally { lifecycle.Release(); }
    }

    private Connection RequireConnection(Guid? expectedProfileId = null)
    {
        var current = connection ?? throw new InvalidOperationException("请先连接");
        if (expectedProfileId is { } profile && profile != current.Profile.Id)
            throw new InvalidOperationException("当前会话属于另一台设备");
        current.Closed.ThrowIfCancellationRequested();
        return current;
    }

    private static ClientSettings CopySettings(ClientSettings settings) =>
        settings with { TouchModes = new(settings.TouchModes, StringComparer.Ordinal) };

    public async Task ApplyAsync(ClientSettings settings, Guid? expectedProfileId = null)
    {
        settings.Validate();
        var current = RequireConnection(expectedProfileId);
        var requested = CopySettings(settings);
        await current.Worker.SendAsync(new { type = "settings", settings = requested }, current.Closed);
        current.Profile.Settings = requested;
    }

    public async Task<IReadOnlyList<RemoteApplication>> ApplicationsAsync(Guid? expectedProfileId = null,
        CancellationToken cancellationToken = default)
    {
        if (!Connected && expectedProfileId is null) return [];
        var current = RequireConnection(expectedProfileId);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, current.Closed);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        var result = await current.Server.CallAsync(new { op = "applications" }, timeout.Token);
        return result.Deserialize<List<RemoteApplication>>(Wire.Json) ?? [];
    }

    public async Task LaunchAsync(string? app = null, string[]? argv = null, Guid? expectedProfileId = null,
        CancellationToken cancellationToken = default, string? action = null, string? cwd = null,
        string[]? extraArgv = null, bool launchWithExec = false)
    {
        var current = RequireConnection(expectedProfileId);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, current.Closed);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        timeout.Token.ThrowIfCancellationRequested();
        var activation = await current.Worker.PrepareActivationAsync(app, timeout.Token);
        timeout.Token.ThrowIfCancellationRequested();
        var result = await current.Server.CallAsync(LaunchRequest("launch", current.Session, app, argv, action, cwd,
            extraArgv, launchWithExec, activation?.Token), timeout.Token);
        if (activation is not null && !current.Closed.IsCancellationRequested)
            await current.Worker.SendAsync(new { type = "activation_launch", request_id = activation.RequestId,
                pid = result.TryGetProperty("pid", out var pid) ? pid.GetInt32() : (int?)null }, timeout.Token);
    }

    public async Task<JsonElement> PreviewLaunchAsync(string? app = null, string[]? argv = null, Guid? expectedProfileId = null,
        CancellationToken cancellationToken = default, string? action = null, string? cwd = null,
        string[]? extraArgv = null, bool launchWithExec = false)
    {
        var current = RequireConnection(expectedProfileId);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, current.Closed);
        timeout.CancelAfter(TimeSpan.FromSeconds(15));
        return await current.Server.CallAsync(LaunchRequest("launch-plan", current.Session, app, argv, action, cwd,
            extraArgv, launchWithExec, null), timeout.Token);
    }

    private static object LaunchRequest(string operation, string session, string? app, string[]? argv, string? action,
        string? cwd, string[]? extraArgv, bool launchWithExec, string? activationToken) => new
    {
        op = operation, session, app, argv, action, cwd,
        extra_argv = extraArgv, launch_mode = launchWithExec ? "exec" : null,
        activation_token = activationToken
    };

    public async Task EndSessionAsync(Guid? expectedProfileId = null, CancellationToken cancellationToken = default)
    {
        await lifecycle.WaitAsync(cancellationToken);
        try
        {
            var current = RequireConnection(expectedProfileId);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(10));
            bool reconnect = current.Profile.AutoReconnect;
            current.Profile.AutoReconnect = false;
            try { await current.Server.CallAsync(new { op = "end", session = current.Session }, timeout.Token); }
            catch { current.Profile.AutoReconnect = reconnect; throw; }
            await StopAsync();
            Status?.Invoke("未连接");
        }
        finally { lifecycle.Release(); }
    }

    public Task SendEngineAsync(object request)
    {
        var current = RequireConnection();
        return current.Worker.SendAsync(request, current.Closed);
    }

    public Task ActivateWindowAsync(uint id, Guid? expectedProfileId = null)
    {
        var current = RequireConnection(expectedProfileId);
        return current.Worker.ActivateWindowAsync(id, current.Closed);
    }

    public ValueTask DisposeAsync() => new(DisconnectAsync());
}
