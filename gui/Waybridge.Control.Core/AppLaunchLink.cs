using System.Buffers.Text;
using System.Diagnostics.CodeAnalysis;
using System.Text;

namespace Waybridge.Control.Core;

public sealed record AppLaunchLink
{
    private const string Prefix = "waybridge://launch/";
    public const int MaxLength = 1450;
    private static readonly UTF8Encoding Utf8 = new(false, true);

    public Guid ProfileId { get; }
    public string AppId { get; }

    private AppLaunchLink(Guid profileId, string appId) => (ProfileId, AppId) = (profileId, appId);

    public static AppLaunchLink Create(Guid profileId, string appId)
    {
        if (profileId == Guid.Empty || !IsAppId(appId))
            throw new ArgumentException("应用链接需要有效的工作站和桌面应用 ID");
        return new(profileId, appId);
    }

    public override string ToString() => $"{Prefix}{ProfileId:N}/{Base64Url.EncodeToString(Utf8.GetBytes(AppId))}";

    public static bool TryParse(string? value, [NotNullWhen(true)] out AppLaunchLink? link)
    {
        link = null;
        if (value is null || value.Length > MaxLength || !value.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase)) return false;
        var path = value.AsSpan(Prefix.Length);
        if (path.Length <= 33 || path[32] != '/' || !Guid.TryParseExact(path[..32], "N", out var profile) ||
            profile == Guid.Empty) return false;
        var encoded = path[33..];
        if (encoded.ContainsAnyExcept("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")) return false;
        try
        {
            var bytes = Base64Url.DecodeFromChars(encoded);
            if (!encoded.SequenceEqual(Base64Url.EncodeToString(bytes))) return false;
            string appId = Utf8.GetString(bytes);
            if (!IsAppId(appId)) return false;
            link = new(profile, appId);
            return true;
        }
        catch (FormatException) { return false; }
        catch (DecoderFallbackException) { return false; }
    }

    private static bool IsAppId(string value)
    {
        if (value.Length is <= 8 or > 1024 || !value.EndsWith(".desktop", StringComparison.Ordinal) ||
            value.Contains('/') || value.Contains('\\') || value.Any(char.IsControl)) return false;
        try { return Utf8.GetByteCount(value) <= 1024; }
        catch (EncoderFallbackException) { return false; }
    }
}
