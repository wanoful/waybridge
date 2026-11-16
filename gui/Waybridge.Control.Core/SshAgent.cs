using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Waybridge.Control.Core;

public static class Processes
{
    public static Process Start(string executable, IEnumerable<string> arguments)
    {
        var info = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var argument in arguments) info.ArgumentList.Add(argument);
        return Process.Start(info) ?? throw new IOException("无法启动 " + Path.GetFileName(executable));
    }
    public static async Task StopAsync(Process? process)
    {
        if (process is null) return;
        try { if (!process.HasExited) process.Kill(true); await process.WaitForExitAsync(); }
        catch (InvalidOperationException) { }
        finally { process.Dispose(); }
    }
    public static async Task<string> ReadErrorAsync(Process process, CancellationToken token)
    {
        char[] buffer = new char[2048];
        var result = new StringBuilder();
        while (true)
        {
            int count = await process.StandardError.ReadAsync(buffer.AsMemory(), token);
            if (count == 0) break;
            result.Append(buffer, 0, count);
            if (result.Length > 16384) result.Remove(0, result.Length - 16384);
        }
        return result.ToString();
    }
}

public sealed class SshAgent : IAsyncDisposable
{
    private static readonly string BootstrapCommand = "python3 -u -c " + PosixQuote(
        "import base64,sys;source=sys.stdin.buffer.readline(1048578);" +
        "(source.endswith(b'\\n') and len(source)<=1048577) or sys.exit('invalid bootstrap size');" +
        "exec(compile(base64.b64decode(source[:-1],validate=True),'<waybridge-session-control>','exec'))");
    private readonly Process process;
    private readonly Task<string> errors;
    private readonly SemaphoreSlim writer = new(1, 1);
    private readonly ConcurrentDictionary<int, TaskCompletionSource<JsonElement>> pending = new();
    private readonly CancellationTokenSource lifetime = new();
    private readonly Task reader;
    private int nextId;
    public Task Exited => process.WaitForExitAsync(lifetime.Token);
    public static string Executable(string name) => OperatingSystem.IsWindows()
        ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "OpenSSH", name + ".exe") : name;
    private SshAgent(Process process)
    {
        this.process = process;
        errors = Processes.ReadErrorAsync(process, lifetime.Token);
        reader = ReadAsync();
    }
    public static string PosixQuote(string value) => "'" + value.Replace("'", "'\"'\"'") + "'";
    public static List<string> Arguments(ConnectionProfile profile, string knownHosts)
    {
        var result = new List<string>
        {
            "-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
            "-o", "ConnectTimeout=12", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
            "-o", "UserKnownHostsFile=\"" + Path.GetFullPath(knownHosts).Replace('\\','/') + "\"",
            "-p", profile.SshPort.ToString()
        };
        if (!string.IsNullOrWhiteSpace(profile.IdentityFile)) result.AddRange(["-i", Path.GetFullPath(profile.IdentityFile)]);
        result.Add((profile.User.Length > 0 ? profile.User + "@" : "") + profile.Host);
        return result;
    }
    public static async Task<SshAgent> StartAsync(ConnectionProfile profile, string directory,
        Func<TrustRequest, Task<bool>> trust, CancellationToken token)
    {
        profile.Validate();
        Directory.CreateDirectory(directory);
        string known = Path.Combine(directory, "known_hosts");
        await VerifyHostAsync(profile, known, trust, token);
        using var resource = Assembly.GetExecutingAssembly().GetManifestResourceStream("Waybridge.SessionControl")
            ?? throw new IOException("会话控制组件缺失");
        using var reader = new StreamReader(resource, Encoding.UTF8);
        string encoded = Convert.ToBase64String(Encoding.UTF8.GetBytes(await reader.ReadToEndAsync(token)));
        // Windows limits the whole CreateProcess command line to 32,767 UTF-16
        // characters. Keep the bootstrap constant-sized as the management
        // script grows; its first stdin line precedes normal JSON RPC framing.
        if (encoded.Length > 1024 * 1024) throw new IOException("会话控制组件过大");
        var args = Arguments(profile, known);
        args.Add(BootstrapCommand);
        var agent = new SshAgent(Processes.Start(Executable("ssh"), args));
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
            timeout.CancelAfter(TimeSpan.FromSeconds(20));
            await agent.process.StandardInput.BaseStream.WriteAsync(Encoding.ASCII.GetBytes(encoded + "\n"), timeout.Token);
            await agent.process.StandardInput.BaseStream.FlushAsync(timeout.Token);
            var hello = await agent.CallAsync(new { op = "hello" }, timeout.Token);
            if (hello.GetProperty("version").GetInt32() != 1) throw new IOException("会话控制版本不兼容");
            return agent;
        }
        catch { await agent.DisposeAsync(); throw; }
    }
    private static async Task VerifyHostAsync(ConnectionProfile profile, string path,
        Func<TrustRequest, Task<bool>> trust, CancellationToken token)
    {
        // Ask OpenSSH to expand Host aliases with exactly the options used below.
        // Only the resolved destination is used by keyscan; the SSH connection
        // still uses the original alias and its IdentityFile/User/Proxy settings.
        var configArgs = Arguments(profile, path); configArgs.Insert(0, "-G");
        using var config = Processes.Start(Executable("ssh"), configArgs);
        using var configTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        configTimeout.CancelAfter(TimeSpan.FromSeconds(12));
        var configErrors = Processes.ReadErrorAsync(config, configTimeout.Token);
        string resolvedHost = profile.Host, hostAlias = "";
        try
        {
            byte[] output = new byte[65536]; int used = 0, count;
            while ((count = await config.StandardOutput.BaseStream.ReadAsync(output.AsMemory(used), configTimeout.Token)) != 0)
            {
                used += count;
                if (used == output.Length) throw new IOException("SSH 配置输出过大");
            }
            await config.WaitForExitAsync(configTimeout.Token);
            if (config.ExitCode != 0) throw new IOException("无法读取 SSH 配置：" + await configErrors);
            foreach (var line in Encoding.UTF8.GetString(output, 0, used).Split('\n'))
            {
                var fields = line.Split(' ', 2);
                if (fields.Length != 2) continue;
                if (fields[0] == "hostname") resolvedHost = fields[1].Trim();
                if (fields[0] == "hostkeyalias" && fields[1].Trim() != "none") hostAlias = fields[1].Trim();
            }
            if (resolvedHost.Length == 0 || resolvedHost.StartsWith('-')) throw new IOException("SSH 主机地址无效");
        }
        finally { if (!config.HasExited) config.Kill(true); }
        // A known host is verified by OpenSSH itself on every connection.
        string hostKey = hostAlias.Length > 0 ? hostAlias : profile.SshPort == 22 ? resolvedHost : "[" + resolvedHost + "]:" + profile.SshPort;
        if (File.Exists(path) && (await File.ReadAllLinesAsync(path, token))
            .Any(line => line.Split(' ', 2)[0].Split(',').Contains(hostKey, StringComparer.OrdinalIgnoreCase))) return;
        using var scan = Processes.Start(Executable("ssh-keyscan"),
            ["-T", "5", "-p", profile.SshPort.ToString(), "-t", "ed25519,ecdsa,rsa", resolvedHost]);
        var errorTask = Processes.ReadErrorAsync(scan, token);
        var lines = new List<string>();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromSeconds(12));
        try
        {
            // ssh-keyscan emits text, not JSON. Read with an explicit total bound.
            byte[] buffer = new byte[65536]; int used = 0, count;
            while ((count = await scan.StandardOutput.BaseStream.ReadAsync(buffer.AsMemory(used), timeout.Token)) != 0)
            {
                used += count;
                if (used == buffer.Length) throw new IOException("SSH 主机密钥响应过大");
            }
            await scan.WaitForExitAsync(timeout.Token);
            foreach (string line in Encoding.UTF8.GetString(buffer, 0, used).Split('\n'))
            {
                var fields = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (fields.Length != 3 || fields[0].StartsWith('#')) continue;
                try
                {
                    byte[] key = Convert.FromBase64String(fields[2]);
                    if (key.Length is < 16 or > 16384) continue;
                    lines.Add(hostKey + " " + fields[1] + " " + fields[2]);
                }
                catch (FormatException) { }
            }
            if (lines.Count == 0) throw new IOException("无法获取 SSH 主机密钥：" + await errorTask);
            string[] fingerprints = lines.Select(line =>
            {
                var fields = line.Split(' ');
                return fields[1] + " SHA256:" + Convert.ToBase64String(SHA256.HashData(Convert.FromBase64String(fields[2]))).TrimEnd('=');
            }).Distinct().ToArray();
            if (!await trust(new(profile.Host, profile.SshPort, fingerprints)).WaitAsync(token)) throw new OperationCanceledException("已取消信任此 SSH 主机");
            await File.AppendAllTextAsync(path, string.Join('\n', lines.Distinct()) + "\n", token);
        }
        finally { if (!scan.HasExited) scan.Kill(true); }
    }
    public async Task<JsonElement> CallAsync(object message, CancellationToken token)
    {
        var source = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        int id = Interlocked.Increment(ref nextId);
        var payload = JsonSerializer.SerializeToElement(message, Wire.Json).EnumerateObject()
            .ToDictionary(p => p.Name, p => p.Value);
        payload["id"] = JsonSerializer.SerializeToElement(id);
        pending[id] = source;
        using var registration = token.Register(() => source.TrySetCanceled(token));
        try
        {
            await writer.WaitAsync(token);
            try { await process.StandardInput.BaseStream.WriteAsync(Wire.Encode(payload), token); await process.StandardInput.BaseStream.FlushAsync(token); }
            finally { writer.Release(); }
            return await source.Task;
        }
        finally { pending.TryRemove(id, out _); }
    }
    private async Task ReadAsync()
    {
        Exception failure;
        try
        {
            var input = new JsonLines(process.StandardOutput.BaseStream);
            while (await input.ReadAsync(lifetime.Token) is { } message)
            {
                if (!message.TryGetProperty("id", out var id) || !id.TryGetInt32(out int number) || !pending.TryRemove(number, out var call)) continue;
                if (message.GetProperty("ok").GetBoolean()) call.TrySetResult(message.GetProperty("result").Clone());
                else call.TrySetException(new IOException(message.GetProperty("error").GetString()));
            }
            await process.WaitForExitAsync(lifetime.Token);
            failure = new IOException("SSH 控制连接已关闭：" + await errors);
        }
        catch (Exception error) { failure = error; }
        foreach (var call in pending.Values) call.TrySetException(failure);
    }
    public async Task<SshTunnel> TunnelAsync(ConnectionProfile profile, string directory, int remotePort, CancellationToken token)
        => await SshTunnel.StartAsync(profile, Path.Combine(directory, "known_hosts"), remotePort, token);
    public async ValueTask DisposeAsync()
    {
        lifetime.Cancel();
        await Processes.StopAsync(process);
        try { await reader; await errors; } catch (Exception error) when (error is OperationCanceledException or IOException or ObjectDisposedException) { }
        lifetime.Dispose();
        writer.Dispose();
    }
}

public sealed class SshTunnel : IAsyncDisposable
{
    private readonly Process process;
    private readonly Task<string> errors;
    private readonly CancellationTokenSource lifetime = new();
    public int Port { get; }
    public Task Exited => process.WaitForExitAsync(lifetime.Token);
    private SshTunnel(Process process, int port)
    {
        this.process = process; Port = port;
        errors = Processes.ReadErrorAsync(process, lifetime.Token);
    }
    public static async Task<SshTunnel> StartAsync(ConnectionProfile profile, string known, int remotePort, CancellationToken token)
    {
        var reserve = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
        reserve.Start(); int localPort = ((System.Net.IPEndPoint)reserve.LocalEndpoint).Port; reserve.Stop();
        var args = SshAgent.Arguments(profile, known);
        string host = args[^1]; args.RemoveAt(args.Count - 1);
        args.AddRange(["-N", "-o", "ExitOnForwardFailure=yes", "-L", $"127.0.0.1:{localPort}:127.0.0.1:{remotePort}", host]);
        var result = new SshTunnel(Processes.Start(SshAgent.Executable("ssh"), args), localPort);
        try
        {
            for (int i = 0; i < 120; i++)
            {
                token.ThrowIfCancellationRequested();
                if (result.process.HasExited) throw new IOException("SSH 隧道失败：" + await result.errors);
                try
                {
                    using var probe = new System.Net.Sockets.TcpClient();
                    await probe.ConnectAsync(System.Net.IPAddress.Loopback, localPort, token);
                    return result;
                }
                catch (System.Net.Sockets.SocketException) { await Task.Delay(100, token); }
            }
            throw new IOException("SSH 隧道未就绪");
        }
        catch { await result.DisposeAsync(); throw; }
    }
    public async ValueTask DisposeAsync()
    {
        lifetime.Cancel(); await Processes.StopAsync(process);
        try { await errors; } catch (Exception error) when (error is OperationCanceledException or IOException or ObjectDisposedException) { }
        lifetime.Dispose();
    }
}
