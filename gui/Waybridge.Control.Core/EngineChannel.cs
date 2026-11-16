using System.Diagnostics;
using System.IO.Pipes;
using System.Text.Json;

namespace Waybridge.Control.Core;

public sealed class EngineChannel : IAsyncDisposable
{
    private readonly NamedPipeServerStream pipe;
    private readonly Process process;
    private readonly CancellationTokenSource lifetime = new();
    private readonly SemaphoreSlim writer = new(1, 1);
    private readonly TaskCompletionSource ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Task reader;
    private readonly Task<string> errors;
    private string secret;
    private string directSecret;
    public event Action<JsonElement>? Message;
    public event Action<string>? Log;
    public Task Exited => process.WaitForExitAsync(lifetime.Token);
    public Task ControlClosed => reader;
    private EngineChannel(NamedPipeServerStream pipe, Process process, string secret, string directSecret)
    {
        this.pipe = pipe; this.process = process; this.secret = secret; this.directSecret = directSecret;
        errors = ReadLogsAsync(process.StandardError);
        _ = ReadLogsAsync(process.StandardOutput);
        reader = ReadAsync();
    }
    private async Task<string> ReadLogsAsync(StreamReader stream)
    {
        string last = "";
        try
        {
            while (await stream.ReadLineAsync(lifetime.Token) is { } line)
            {
                string sessionSecret = secret, clientSecret = directSecret;
                if (sessionSecret.Length > 0) line = line.Replace(sessionSecret, "[令牌已隐藏]", StringComparison.Ordinal);
                if (clientSecret.Length > 0) line = line.Replace(clientSecret, "[客户端凭据已隐藏]", StringComparison.Ordinal);
                last = line.Length > 2048 ? line[..2048] : line;
                Log?.Invoke(last);
            }
        }
        catch (Exception error) when (error is IOException or OperationCanceledException or ObjectDisposedException) { }
        return last;
    }
    public static async Task<EngineChannel> StartAsync(string executable, string host, int port, string token,
        ClientSettings settings, Action<JsonElement> message, Action<string> log,
        CancellationToken cancellationToken, IReadOnlyList<string>? prefixArguments = null, DirectTransport? transport = null, string sourceId = "")
    {
        if (token.Length is < 16 or > 4096 || token.Any(char.IsWhiteSpace)) throw new ArgumentException("请输入有效的会话令牌");
        string name = "waybridge-" + Guid.NewGuid().ToString("N");
        var pipe = new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        Process? process = null;
        EngineChannel? channel = null;
        try
        {
            var arguments = new List<string>(prefixArguments ?? []);
            arguments.AddRange(["--control-pipe", name]);
            process = Processes.Start(executable, arguments);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            var connected = pipe.WaitForConnectionAsync(timeout.Token);
            var exited = process.WaitForExitAsync(timeout.Token);
            if (await Task.WhenAny(connected, exited) == exited)
            {
                await exited; // A cancelled wait does not mean the process exited.
                throw new IOException("串流内核在连接控制通道前退出");
            }
            await connected;
            channel = new EngineChannel(pipe, process, token, transport?.ClientToken ?? "");
            channel.Message += message; channel.Log += log;
            await channel.SendAsync(new { type = "start", version = 1, host, port, token, settings, transport, source_id = sourceId }, timeout.Token);
            var engineReady = channel.ready.Task;
            if (await Task.WhenAny(engineReady, exited) == exited)
            {
                await exited;
                throw new IOException("串流连接失败：" + await channel.errors.WaitAsync(timeout.Token));
            }
            await engineReady.WaitAsync(timeout.Token);
            return channel;
        }
        catch (Exception error)
        {
            if (channel is not null) await channel.DisposeAsync();
            else { pipe.Dispose(); await Processes.StopAsync(process); }
            if (error is OperationCanceledException && !cancellationToken.IsCancellationRequested)
                throw new TimeoutException("串流内核未在 30 秒内就绪，请查看诊断日志", error);
            throw;
        }
    }
    public async Task SendAsync(object message, CancellationToken token = default)
    {
        byte[] bytes = Wire.Encode(message);
        if (bytes.Length > 65536) throw new ArgumentException("控制请求过大");
        await writer.WaitAsync(token);
        try { await pipe.WriteAsync(bytes, token); await pipe.FlushAsync(token); }
        finally { writer.Release(); }
    }
    private async Task ReadAsync()
    {
        try
        {
            var input = new JsonLines(pipe);
            while (await input.ReadAsync(lifetime.Token) is { } message)
            {
                if (message.GetProperty("type").GetString() == "connected") ready.TrySetResult();
                if (message.GetProperty("type").GetString() == "error" && !ready.Task.IsCompleted)
                    ready.TrySetException(new IOException(message.GetProperty("message").GetString()));
                Message?.Invoke(message);
            }
            ready.TrySetException(new IOException("串流控制通道已关闭"));
        }
        catch (Exception error) { ready.TrySetException(error); }
    }
    public async ValueTask DisposeAsync()
    {
        try
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            if (pipe.IsConnected && !process.HasExited)
            {
                await SendAsync(new { type = "disconnect" }, timeout.Token);
                await process.WaitForExitAsync(timeout.Token);
            }
        }
        catch (Exception error) when (error is IOException or OperationCanceledException or InvalidOperationException) { }
        lifetime.Cancel(); pipe.Dispose();
        await Processes.StopAsync(process);
        try { await reader; await errors; } catch (OperationCanceledException) { }
        secret = ""; directSecret = ""; writer.Dispose(); lifetime.Dispose();
    }
}
