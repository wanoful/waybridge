using System.Buffers.Binary;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

namespace Waybridge.Control.Core;

public readonly record struct ShellFrame(byte Kind, byte[] Payload);

public static class ShellProtocol
{
    public const byte KindData = 1;
    public const byte KindResize = 2;
    public const byte KindExit = 3;
    public const byte KindPing = 4;
    public const byte KindPong = 5;

    public const byte ReasonExited = 0;
    public const byte ReasonSessionEnded = 1;
    public const byte ReasonRevoked = 2;

    public const int MaxFrameBytes = 65536;

    public static async Task WriteFrameAsync(Stream stream, byte kind, ReadOnlyMemory<byte> payload, CancellationToken cancellationToken)
    {
        if (payload.Length > MaxFrameBytes)
            throw new ArgumentOutOfRangeException(nameof(payload), "Shell frame exceeds maximum payload size.");
        byte[] header = new byte[5];
        header[0] = kind;
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(1, 4), (uint)payload.Length);
        await stream.WriteAsync(header, cancellationToken);
        if (!payload.IsEmpty)
            await stream.WriteAsync(payload, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    public static Task WriteResizeAsync(Stream stream, ushort cols, ushort rows, CancellationToken cancellationToken)
    {
        byte[] payload = new byte[4];
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(0, 2), cols);
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(2, 2), rows);
        return WriteFrameAsync(stream, KindResize, payload, cancellationToken);
    }

    public static async Task<ShellFrame?> ReadFrameAsync(Stream stream, CancellationToken cancellationToken)
    {
        byte[] header = new byte[5];
        if (!await ReadExactOrEofAsync(stream, header, cancellationToken)) return null;
        byte kind = header[0];
        uint length = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(1, 4));
        if (length > MaxFrameBytes) throw new IOException("Shell frame exceeds limit.");
        byte[] payload = length > 0 ? new byte[length] : [];
        if (length > 0 && !await ReadExactOrEofAsync(stream, payload, cancellationToken))
            throw new EndOfStreamException("Shell stream ended mid-frame.");
        return new ShellFrame(kind, payload);
    }

    public static (int ExitCode, byte Reason) ParseExit(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != 5) throw new IOException("Invalid shell exit frame.");
        return (BinaryPrimitives.ReadInt32BigEndian(payload[..4]), payload[4]);
    }

    private static async Task<bool> ReadExactOrEofAsync(Stream stream, byte[] buffer, CancellationToken cancellationToken)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int count = await stream.ReadAsync(buffer.AsMemory(offset, buffer.Length - offset), cancellationToken);
            if (count == 0)
            {
                if (offset == 0) return false;
                throw new EndOfStreamException("Shell stream ended unexpectedly.");
            }
            offset += count;
        }
        return true;
    }
}

public sealed class ShellConnection : IAsyncDisposable
{
    private readonly TcpClient client;
    private readonly SslStream tls;
    private readonly SemaphoreSlim writeLock = new(1, 1);

    private ShellConnection(TcpClient client, SslStream tls, Stream framedStream)
    {
        this.client = client;
        this.tls = tls;
        FramedStream = framedStream;
    }

    public Stream FramedStream { get; }

    public static async Task<ShellConnection> OpenAsync(
        ConnectionProfile profile,
        string clientToken,
        string session,
        ushort cols,
        ushort rows,
        string term = "xterm-256color",
        string? cwd = null,
        CancellationToken cancellationToken = default)
    {
        profile = profile with { };
        profile.Validate();
        if (profile.CertificateSha256.Length != 64 || !profile.CertificateSha256.All(Uri.IsHexDigit))
            throw new ArgumentException(Tr("Enter and verify the 64-character SHA-256 certificate fingerprint of the Linux service"));
        if (string.IsNullOrWhiteSpace(profile.ClientId) || clientToken.Length is < 32 or > 4096 || clientToken.Any(char.IsWhiteSpace))
            throw new ArgumentException(Tr("Pair with this Linux service or restore the saved device credentials"));

        var client = new TcpClient { NoDelay = true };
        SslStream? tls = null;
        try
        {
            using var handshakeDeadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            handshakeDeadline.CancelAfter(TimeSpan.FromSeconds(15));
            await client.ConnectAsync(profile.Address, profile.Port, handshakeDeadline.Token);
            byte[] expected = Convert.FromHexString(profile.CertificateSha256);
            tls = new SslStream(client.GetStream(), false, (_, certificate, _, _) =>
                certificate is not null && CryptographicOperations.FixedTimeEquals(expected, SHA256.HashData(certificate.GetRawCertData())));
            await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
            {
                TargetHost = profile.Address,
                EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                CertificateRevocationCheckMode = X509RevocationMode.NoCheck,
                AllowRenegotiation = false
            }, handshakeDeadline.Token);

            var request = new Dictionary<string, object?>
            {
                ["id"] = 1,
                ["op"] = "shell",
                ["client_id"] = profile.ClientId,
                ["client_token"] = clientToken,
                ["session"] = session,
                ["cols"] = (int)Math.Clamp(cols, (ushort)1, (ushort)1000),
                ["rows"] = (int)Math.Clamp(rows, (ushort)1, (ushort)500),
                ["term"] = string.IsNullOrWhiteSpace(term) ? "xterm-256color" : term,
            };
            if (!string.IsNullOrWhiteSpace(cwd)) request["cwd"] = cwd;

            byte[] encoded = Wire.Encode(request);
            await tls.WriteAsync(encoded, handshakeDeadline.Token);
            await tls.FlushAsync(handshakeDeadline.Token);

            var lines = new JsonLines(tls, 65536);
            var response = await lines.ReadAsync(handshakeDeadline.Token)
                ?? throw new IOException(Tr("The server did not return a complete response"));
            if (!response.TryGetProperty("ok", out var ok) || !ok.GetBoolean())
            {
                string message = response.TryGetProperty("error", out var error) &&
                    error.ValueKind == JsonValueKind.Object &&
                    error.TryGetProperty("message", out var detail)
                    ? detail.GetString() ?? Tr("Operation failed")
                    : Tr("Operation failed");
                if (error.ValueKind == JsonValueKind.Object &&
                    error.TryGetProperty("code", out var code) &&
                    code.GetString() == "unauthorized")
                    throw new AuthenticationException(Tr("The Linux service rejected the device credentials. Pair again."));
                throw new IOException(Tr("Linux service: ") + message[..Math.Min(message.Length, 2048)]);
            }
            if (!response.TryGetProperty("result", out var result) ||
                !result.TryGetProperty("shell_ready", out var ready) || !ready.GetBoolean())
                throw new IOException(Tr("The Linux service did not start the shell channel."));

            byte[] leftover = lines.TakeBufferedBytes();
            Stream framed = leftover.Length > 0 ? new PrefixedStream(tls, leftover) : tls;
            return new ShellConnection(client, tls, framed);
        }
        catch
        {
            if (tls is not null) await tls.DisposeAsync();
            client.Dispose();
            throw;
        }
    }

    public async Task WriteDataAsync(ReadOnlyMemory<byte> data, CancellationToken cancellationToken)
    {
        await writeLock.WaitAsync(cancellationToken);
        try { await ShellProtocol.WriteFrameAsync(FramedStream, ShellProtocol.KindData, data, cancellationToken); }
        finally { writeLock.Release(); }
    }

    public async Task WriteResizeAsync(ushort cols, ushort rows, CancellationToken cancellationToken)
    {
        await writeLock.WaitAsync(cancellationToken);
        try { await ShellProtocol.WriteResizeAsync(FramedStream, cols, rows, cancellationToken); }
        finally { writeLock.Release(); }
    }

    public async Task WritePingAsync(CancellationToken cancellationToken)
    {
        await writeLock.WaitAsync(cancellationToken);
        try { await ShellProtocol.WriteFrameAsync(FramedStream, ShellProtocol.KindPing, ReadOnlyMemory<byte>.Empty, cancellationToken); }
        finally { writeLock.Release(); }
    }

    public Task<ShellFrame?> ReadFrameAsync(CancellationToken cancellationToken) =>
        ShellProtocol.ReadFrameAsync(FramedStream, cancellationToken);

    public async ValueTask DisposeAsync()
    {
        try { await tls.DisposeAsync(); } catch { }
        client.Dispose();
        writeLock.Dispose();
    }

    private sealed class PrefixedStream(Stream inner, byte[] prefix) : Stream
    {
        private int offset;
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override void Flush() => inner.Flush();
        public override Task FlushAsync(CancellationToken cancellationToken) => inner.FlushAsync(cancellationToken);
        public override int Read(byte[] buffer, int index, int count)
        {
            if (offset < prefix.Length)
            {
                int copy = Math.Min(count, prefix.Length - offset);
                Buffer.BlockCopy(prefix, offset, buffer, index, copy);
                offset += copy;
                return copy;
            }
            return inner.Read(buffer, index, count);
        }
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            if (offset < prefix.Length)
            {
                int copy = Math.Min(buffer.Length, prefix.Length - offset);
                prefix.AsMemory(offset, copy).CopyTo(buffer);
                offset += copy;
                return ValueTask.FromResult(copy);
            }
            return inner.ReadAsync(buffer, cancellationToken);
        }
        public override void Write(byte[] buffer, int index, int count) => inner.Write(buffer, index, count);
        public override ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default) =>
            inner.WriteAsync(buffer, cancellationToken);
        public override long Seek(long o, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
    }
}
