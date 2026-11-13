using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

namespace Waybridge.Control.Core;

public sealed record DirectCredentials(string ClientId, string ClientToken);
public sealed record DirectTransport(string Fingerprint, string ClientId, string ClientToken, string Session)
{
    public string Type => "tls";
}

/// Management RPC only. Media stays in the native worker's pinned TLS streams.
public sealed class DirectAgent
{
    private readonly ConnectionProfile profile;
    private readonly string clientToken;
    private int nextId;

    public DirectAgent(ConnectionProfile profile, string clientToken)
    {
        RequirePin(profile.CertificateSha256);
        if (string.IsNullOrWhiteSpace(profile.ClientId) || clientToken.Length is < 32 or > 4096 || clientToken.Any(char.IsWhiteSpace))
            throw new ArgumentException("请先配对此 Linux 服务，或恢复已保存的客户端凭据");
        this.profile = profile;
        this.clientToken = clientToken;
    }

    public DirectTransport Transport(string session) => new(profile.CertificateSha256.ToLowerInvariant(), profile.ClientId, clientToken, session);

    public static async Task<DirectCredentials> PairAsync(ConnectionProfile profile, string code, string label, CancellationToken token = default)
    {
        RequirePin(profile.CertificateSha256);
        if (code.Length is < 16 or > 4096 || code.Any(char.IsWhiteSpace)) throw new ArgumentException("请输入 Linux 服务生成的配对码");
        if (string.IsNullOrWhiteSpace(label) || System.Text.Encoding.UTF8.GetByteCount(label) > 128) throw new ArgumentException("设备名称过长");
        var result = await RequestAsync(profile, new { id = 1, op = "pair", pair_code = code, label }, token);
        string id = result.GetProperty("client_id").GetString()!;
        string secret = result.GetProperty("client_token").GetString()!;
        if (string.IsNullOrWhiteSpace(id) || secret.Length is < 32 or > 4096 || secret.Any(char.IsWhiteSpace)) throw new IOException("服务端返回了无效的配对结果");
        if (!string.Equals(result.GetProperty("certificate_sha256").GetString(), profile.CertificateSha256, StringComparison.OrdinalIgnoreCase))
            throw new AuthenticationException("配对结果的服务端身份不匹配");
        return new(id, secret);
    }

    public async Task<JsonElement> CallAsync(object request, CancellationToken token)
    {
        var fields = JsonSerializer.SerializeToElement(request, Wire.Json).EnumerateObject().ToDictionary(p => p.Name, p => p.Value);
        fields["id"] = JsonSerializer.SerializeToElement(Interlocked.Increment(ref nextId));
        fields["client_id"] = JsonSerializer.SerializeToElement(profile.ClientId);
        fields["client_token"] = JsonSerializer.SerializeToElement(clientToken);
        return await RequestAsync(profile, fields, token);
    }

    private static void RequirePin(string pin)
    {
        if (pin.Length != 64 || !pin.All(Uri.IsHexDigit)) throw new ArgumentException("请输入并核对 Linux 服务的 64 位 SHA-256 证书指纹");
    }

    private static async Task<JsonElement> RequestAsync(ConnectionProfile profile, object request, CancellationToken token)
    {
        RequirePin(profile.CertificateSha256);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(TimeSpan.FromSeconds(40));
        using var client = new TcpClient { NoDelay = true };
        await client.ConnectAsync(profile.Host.Trim('[', ']'), profile.Port, deadline.Token);
        byte[] expected = Convert.FromHexString(profile.CertificateSha256);
        using var tls = new SslStream(client.GetStream(), false, (_, certificate, _, _) =>
            certificate is not null && CryptographicOperations.FixedTimeEquals(expected, SHA256.HashData(certificate.GetRawCertData())));
        using (var handshake = CancellationTokenSource.CreateLinkedTokenSource(deadline.Token))
        {
            handshake.CancelAfter(TimeSpan.FromSeconds(10));
            await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
            {
                TargetHost = profile.Host.Trim('[', ']'),
                EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                CertificateRevocationCheckMode = X509RevocationMode.NoCheck,
                AllowRenegotiation = false
            }, handshake.Token);
        }
        byte[] bytes = Wire.Encode(request);
        if (bytes.Length > 65536) throw new ArgumentException("服务管理请求过大");
        await tls.WriteAsync(bytes, deadline.Token);
        await tls.FlushAsync(deadline.Token);
        var response = await new JsonLines(tls, 4 * 1024 * 1024).ReadAsync(deadline.Token)
            ?? throw new IOException("服务端未返回完整响应");
        var sent = JsonSerializer.SerializeToElement(request, Wire.Json);
        if (!response.TryGetProperty("id", out var responseId) || responseId.GetRawText() != sent.GetProperty("id").GetRawText())
            throw new IOException("服务管理响应标识不匹配");
        if (!response.GetProperty("ok").GetBoolean())
        {
            string message = response.TryGetProperty("error", out var error) && error.TryGetProperty("message", out var detail)
                ? detail.GetString() ?? "操作失败" : "操作失败";
            if (error.ValueKind == JsonValueKind.Object && error.TryGetProperty("code", out var code) && code.GetString() == "unauthorized")
                throw new AuthenticationException("Linux 服务拒绝了客户端凭据，请重新配对");
            throw new IOException("Linux 服务：" + message[..Math.Min(message.Length, 2048)]);
        }
        return response.GetProperty("result").Clone();
    }
}
