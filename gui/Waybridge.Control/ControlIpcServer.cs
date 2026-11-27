using System.IO.Pipes;
using System.Text.Json;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed class ControlIpcServer : IAsyncDisposable
{
    private sealed record Subscriber(Guid ProfileId, NamedPipeServerStream Pipe, CancellationTokenSource Cancel);

    private readonly string pipeName;
    private readonly Func<Guid, CancellationToken, Task<string>> ensureConnected;
    private readonly Func<Guid, bool> isProfileConnected;
    private readonly CancellationTokenSource shutdown = new();
    private readonly object gate = new();
    private readonly List<Subscriber> subscribers = [];
    private readonly Task acceptLoop;

    public ControlIpcServer(
        string stateDirectory,
        Func<Guid, CancellationToken, Task<string>> ensureConnected,
        Func<Guid, bool> isProfileConnected)
    {
        pipeName = ControlIpc.PipeName(stateDirectory);
        this.ensureConnected = ensureConnected;
        this.isProfileConnected = isProfileConnected;
        acceptLoop = Task.Run(AcceptLoopAsync);
    }

    public void NotifyDisconnected(string reason)
    {
        List<Subscriber> snapshot;
        lock (gate)
        {
            if (subscribers.Count == 0) return;
            snapshot = [.. subscribers];
            subscribers.Clear();
        }
        byte[] payload = Wire.Encode(new { @event = "disconnected", reason });
        foreach (var sub in snapshot)
        {
            _ = Task.Run(async () =>
            {
                try
                {
                    using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(500));
                    await sub.Pipe.WriteAsync(payload, deadline.Token);
                    await sub.Pipe.FlushAsync(deadline.Token);
                }
                catch { }
                finally
                {
                    sub.Cancel.Cancel();
                    try { await sub.Pipe.DisposeAsync(); } catch { }
                    sub.Cancel.Dispose();
                }
            });
        }
    }

    private async Task AcceptLoopAsync()
    {
        while (!shutdown.IsCancellationRequested)
        {
            NamedPipeServerStream? pipe = null;
            try
            {
                pipe = new NamedPipeServerStream(
                    pipeName,
                    PipeDirection.InOut,
                    NamedPipeServerStream.MaxAllowedServerInstances,
                    PipeTransmissionMode.Byte,
                    PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await pipe.WaitForConnectionAsync(shutdown.Token);
                var accepted = pipe;
                pipe = null;
                _ = Task.Run(() => HandleClientAsync(accepted));
            }
            catch (OperationCanceledException) when (shutdown.IsCancellationRequested)
            {
                if (pipe is not null) await pipe.DisposeAsync();
                break;
            }
            catch
            {
                if (pipe is not null) await pipe.DisposeAsync();
                try { await Task.Delay(200, shutdown.Token); }
                catch (OperationCanceledException) { break; }
            }
        }
    }

    private async Task HandleClientAsync(NamedPipeServerStream pipe)
    {
        Subscriber? subscriber = null;
        try
        {
            var lines = new JsonLines(pipe, 65536);
            JsonElement? request;
            using (var readTimeout = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token))
            {
                readTimeout.CancelAfter(TimeSpan.FromSeconds(10));
                request = await lines.ReadAsync(readTimeout.Token);
            }
            if (request is null) return;

            string? op = request.Value.TryGetProperty("op", out var opProp) && opProp.ValueKind == JsonValueKind.String
                ? opProp.GetString() : null;
            if (op != "ensure_connected")
            {
                await WriteReplyAsync(pipe, new { ok = false, error = Tr("Unknown control request.") }, shutdown.Token);
                return;
            }
            string? idText = request.Value.TryGetProperty("profile_id", out var idProp) && idProp.ValueKind == JsonValueKind.String
                ? idProp.GetString() : null;
            if (!Guid.TryParse(idText, out Guid profileId) || profileId == Guid.Empty)
            {
                await WriteReplyAsync(pipe, new { ok = false, error = Tr("Invalid connection ID.") }, shutdown.Token);
                return;
            }

            string session;
            try
            {
                session = await ensureConnected(profileId, shutdown.Token);
            }
            catch (Exception error)
            {
                await WriteReplyAsync(pipe, new { ok = false, error = error.Message }, shutdown.Token);
                return;
            }

            var subCancel = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
            lock (gate)
            {
                if (shutdown.IsCancellationRequested || !isProfileConnected(profileId))
                {
                    subCancel.Dispose();
                    throw new IOException(Tr("Disconnected from the remote workstation."));
                }
                subscriber = new Subscriber(profileId, pipe, subCancel);
                subscribers.Add(subscriber);
            }

            await WriteReplyAsync(pipe, new { ok = true, session }, subCancel.Token);

            // Keep pipe open until CLI exits or NotifyDisconnected closes it.
            byte[] drain = new byte[64];
            while (!subCancel.IsCancellationRequested)
            {
                int count = await pipe.ReadAsync(drain, subCancel.Token);
                if (count == 0) break;
            }
        }
        catch { }
        finally
        {
            if (subscriber is not null)
            {
                bool removed;
                lock (gate) removed = subscribers.Remove(subscriber);
                if (removed)
                {
                    subscriber.Cancel.Dispose();
                    try { await pipe.DisposeAsync(); } catch { }
                }
            }
            else
            {
                try { await pipe.DisposeAsync(); } catch { }
            }
        }
    }

    private static async Task WriteReplyAsync(Stream pipe, object payload, CancellationToken cancellationToken)
    {
        byte[] encoded = Wire.Encode(payload);
        await pipe.WriteAsync(encoded, cancellationToken);
        await pipe.FlushAsync(cancellationToken);
    }

    public async ValueTask DisposeAsync()
    {
        shutdown.Cancel();
        NotifyDisconnected(Tr("Waybridge Control Center closed."));
        try { await acceptLoop; } catch { }
        shutdown.Dispose();
    }
}
