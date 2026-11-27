using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Waybridge.Control.Core;

public static class ControlIpc
{
    public static string PipeName(string stateDirectory)
    {
        string normalized = Path.GetFullPath(stateDirectory).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        if (OperatingSystem.IsWindows()) normalized = normalized.ToUpperInvariant();
        string hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(normalized)))[..16].ToLowerInvariant();
        return "Waybridge.ControlCenter.Ipc." + hash;
    }
}

public sealed class ControlIpcClient : IAsyncDisposable
{
    private readonly NamedPipeClientStream pipe;
    private readonly JsonLines lines;

    private ControlIpcClient(NamedPipeClientStream pipe, JsonLines lines, string session)
    {
        this.pipe = pipe;
        this.lines = lines;
        Session = session;
    }

    public string Session { get; }

    public static async Task<ControlIpcClient> ConnectAndEnsureAsync(
        string pipeName,
        Guid profileId,
        TimeSpan connectTimeout,
        CancellationToken cancellationToken)
    {
        var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        try
        {
            using (var connectCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
            {
                connectCts.CancelAfter(connectTimeout);
                await pipe.ConnectAsync(connectCts.Token);
            }

            byte[] request = Wire.Encode(new { op = "ensure_connected", profile_id = profileId.ToString("D") });
            await pipe.WriteAsync(request, cancellationToken);
            await pipe.FlushAsync(cancellationToken);

            var lines = new JsonLines(pipe, 65536);
            var reply = await lines.ReadAsync(cancellationToken)
                ?? throw new IOException(Tr("Waybridge Control Center closed the connection unexpectedly."));
            if (!reply.TryGetProperty("ok", out var ok) || !ok.GetBoolean())
            {
                string message = reply.TryGetProperty("error", out var error) && error.ValueKind == JsonValueKind.String
                    ? error.GetString() ?? Tr("Could not connect to the workstation.")
                    : Tr("Could not connect to the workstation.");
                throw new InvalidOperationException(message);
            }
            string session = reply.TryGetProperty("session", out var value) && value.ValueKind == JsonValueKind.String
                ? value.GetString() ?? ""
                : "";
            if (string.IsNullOrWhiteSpace(session))
                throw new IOException(Tr("Waybridge Control Center did not return a session name."));
            return new ControlIpcClient(pipe, lines, session);
        }
        catch
        {
            await pipe.DisposeAsync();
            throw;
        }
    }

    public async Task<string> WaitForDisconnectAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (true)
            {
                var message = await lines.ReadAsync(cancellationToken);
                if (message is null) return Tr("Connection closed.");
                if (message.Value.TryGetProperty("event", out var evt) && evt.GetString() == "disconnected")
                {
                    if (message.Value.TryGetProperty("reason", out var reason) &&
                        reason.ValueKind == JsonValueKind.String &&
                        !string.IsNullOrWhiteSpace(reason.GetString()))
                        return reason.GetString()!;
                    return Tr("Disconnected from the remote workstation.");
                }
            }
        }
        catch (IOException)
        {
            return Tr("Connection closed.");
        }
    }

    public ValueTask DisposeAsync() => pipe.DisposeAsync();
}
