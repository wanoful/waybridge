using System.Text.Json;

namespace Waybridge.Control.Core;

public sealed record RemoteMediaState(
    ulong Epoch,
    string Player,
    ulong Generation,
    string Identity,
    string DesktopEntry,
    string Status,
    string Title,
    IReadOnlyList<string> Artists,
    string Album,
    IReadOnlyList<string> AlbumArtists,
    string TrackId,
    long LengthUs,
    long? PositionUs,
    double Rate,
    bool CanControl,
    bool CanPlay,
    bool CanPause,
    bool CanGoNext,
    bool CanGoPrevious,
    bool CanSeek,
    ulong ArtRevision,
    bool ReadOnly)
{
    public string DisplayTitle =>
        Title.Length > 0 ? Title :
        Identity.Length > 0 ? Identity :
        MediaControlsModel.ShortPlayerName(Player);

    public string DisplayArtist =>
        Artists.Count > 0 ? string.Join(", ", Artists) :
        Title.Length > 0 ? Identity : "";

    public string DisplayAlbum => Album;

    public bool EffectiveCanPlay => CanControl && CanPlay && !ReadOnly;
    public bool EffectiveCanPause => CanControl && CanPause && !ReadOnly;
    public bool EffectiveCanStop => CanControl && !ReadOnly;
    public bool EffectiveCanNext => CanControl && CanGoNext && !ReadOnly;
    public bool EffectiveCanPrevious => CanControl && CanGoPrevious && !ReadOnly;
    public bool EffectiveCanSeek => CanControl && CanSeek && !ReadOnly && LengthUs > 0 && TrackId.Length > 0;
}

public static class MediaControlsModel
{
    private const string MprisPrefix = "org.mpris.MediaPlayer2.";
    private const long MaxDurationUs = 86_400_000_000_000;
    private const int MaxArtBytes = 256 * 1024;
    private const int MaxArtBase64Length = 360 * 1024;

    public static string ShortPlayerName(string player)
    {
        if (!player.StartsWith(MprisPrefix, StringComparison.Ordinal)) return player;
        string tail = player[MprisPrefix.Length..];
        int dot = tail.IndexOf('.');
        return dot > 0 ? tail[..dot] : tail;
    }

    public static bool TryParseState(JsonElement message, out RemoteMediaState? state)
    {
        state = null;
        if (message.ValueKind != JsonValueKind.Object) return false;
        if (!message.TryGetProperty("type", out var typeProp) || typeProp.GetString() != "media_state")
            return false;
        if (!TryGetUInt64(message, "epoch", out ulong epoch) || epoch == 0) return false;
        if (!TryGetUInt64(message, "generation", out ulong generation) || generation == 0) return false;
        string player = GetString(message, "player", 255);
        if (!player.StartsWith(MprisPrefix, StringComparison.Ordinal)) return false;
        string status = GetString(message, "status", 32);
        if (status is not ("Playing" or "Paused" or "Stopped")) return false;

        string identity = GetString(message, "identity", 256);
        string desktopEntry = GetString(message, "desktop_entry", 255);
        string title = GetString(message, "title", 512);
        string album = GetString(message, "album", 512);
        string trackId = GetString(message, "track_id", 512);
        var artists = GetStringList(message, "artists", 8, 256);
        var albumArtists = GetStringList(message, "album_artists", 8, 256);

        long lengthUs = GetInt64(message, "length_us") is { } len and >= 0 and <= MaxDurationUs ? len : 0;
        long? positionUs = GetInt64(message, "position_us") is { } pos and >= 0 and <= MaxDurationUs ? pos : null;
        double rate = message.TryGetProperty("rate", out var rateProp) && rateProp.ValueKind == JsonValueKind.Number &&
            rateProp.TryGetDouble(out double parsedRate) && double.IsFinite(parsedRate) && parsedRate > 0 && parsedRate <= 16
            ? parsedRate : 1.0;

        bool canControl = GetBool(message, "can_control");
        bool canPlay = canControl && GetBool(message, "can_play");
        bool canPause = canControl && GetBool(message, "can_pause");
        bool canGoNext = canControl && GetBool(message, "can_go_next");
        bool canGoPrevious = canControl && GetBool(message, "can_go_previous");
        bool canSeek = canControl && GetBool(message, "can_seek");
        bool readOnly = GetBool(message, "read_only");
        ulong artRevision = TryGetUInt64(message, "art_revision", out ulong rev) ? rev : 0;

        state = new RemoteMediaState(
            epoch, player, generation, identity, desktopEntry, status, title,
            artists, album, albumArtists, trackId, lengthUs, positionUs, rate,
            canControl, canPlay, canPause, canGoNext, canGoPrevious, canSeek,
            artRevision, readOnly);
        return true;
    }

    public static bool TryParseArt(JsonElement message, RemoteMediaState? current,
        out byte[] bytes, out string mime, out ulong revision)
    {
        bytes = [];
        mime = "";
        revision = 0;
        if (current is null || current.ArtRevision == 0 || message.ValueKind != JsonValueKind.Object)
            return false;
        if (!message.TryGetProperty("type", out var typeProp) || typeProp.GetString() != "media_art")
            return false;
        if (!TryGetUInt64(message, "epoch", out ulong epoch) || epoch != current.Epoch) return false;
        if (!TryGetUInt64(message, "generation", out ulong generation) || generation != current.Generation) return false;
        if (GetString(message, "player", 255) != current.Player) return false;
        if (!TryGetUInt64(message, "revision", out revision) || revision != current.ArtRevision) return false;
        mime = GetString(message, "mime", 32);
        if (mime is not ("image/png" or "image/jpeg" or "image/webp")) return false;
        if (!message.TryGetProperty("data", out var dataProp) || dataProp.ValueKind != JsonValueKind.String)
            return false;
        string? encoded = dataProp.GetString();
        if (string.IsNullOrEmpty(encoded) || encoded.Length > MaxArtBase64Length) return false;
        try { bytes = Convert.FromBase64String(encoded); }
        catch (FormatException) { return false; }
        if (bytes.Length < 12 || bytes.Length > MaxArtBytes) return false;
        return MatchesMagic(bytes, mime);
    }

    public static Dictionary<string, object?>? BuildActionCommand(RemoteMediaState? state, string action)
    {
        if (state is null || state.ReadOnly || !state.CanControl) return null;
        bool allowed = action switch
        {
            "play" => state.EffectiveCanPlay,
            "pause" => state.EffectiveCanPause,
            "play_pause" => state.EffectiveCanPlay || state.EffectiveCanPause,
            "stop" => state.EffectiveCanStop,
            "next" => state.EffectiveCanNext,
            "previous" => state.EffectiveCanPrevious,
            _ => false
        };
        if (!allowed) return null;
        return new Dictionary<string, object?>
        {
            ["type"] = "media_control",
            ["epoch"] = state.Epoch,
            ["player"] = state.Player,
            ["generation"] = state.Generation,
            ["action"] = action
        };
    }

    public static Dictionary<string, object?>? BuildSeekCommand(RemoteMediaState? state, long positionUs)
    {
        if (state is null || !state.EffectiveCanSeek) return null;
        long clamped = Math.Clamp(positionUs, 0, state.LengthUs);
        return new Dictionary<string, object?>
        {
            ["type"] = "media_control",
            ["epoch"] = state.Epoch,
            ["player"] = state.Player,
            ["generation"] = state.Generation,
            ["action"] = "set_position",
            ["track_id"] = state.TrackId,
            ["position_us"] = clamped
        };
    }

    private static bool MatchesMagic(byte[] bytes, string mime) => mime switch
    {
        "image/png" => bytes.Length >= 8 && bytes.AsSpan(0, 8).SequenceEqual(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 }),
        "image/jpeg" => bytes.Length >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF,
        "image/webp" => bytes.Length >= 12 &&
            bytes[0] == (byte)'R' && bytes[1] == (byte)'I' && bytes[2] == (byte)'F' && bytes[3] == (byte)'F' &&
            bytes[8] == (byte)'W' && bytes[9] == (byte)'E' && bytes[10] == (byte)'B' && bytes[11] == (byte)'P',
        _ => false
    };

    private static bool TryGetUInt64(JsonElement obj, string name, out ulong value)
    {
        value = 0;
        return obj.TryGetProperty(name, out var prop) && prop.ValueKind == JsonValueKind.Number && prop.TryGetUInt64(out value);
    }

    private static long? GetInt64(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var prop) && prop.ValueKind == JsonValueKind.Number && prop.TryGetInt64(out long value)
            ? value : null;

    private static bool GetBool(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var prop) && prop.ValueKind == JsonValueKind.True;

    private static string GetString(JsonElement obj, string name, int limit)
    {
        if (!obj.TryGetProperty(name, out var prop) || prop.ValueKind != JsonValueKind.String) return "";
        string raw = prop.GetString() ?? "";
        return raw.Length > limit ? raw[..limit] : raw;
    }

    private static List<string> GetStringList(JsonElement obj, string name, int maxItems, int maxChars)
    {
        var list = new List<string>();
        if (!obj.TryGetProperty(name, out var prop) || prop.ValueKind != JsonValueKind.Array) return list;
        foreach (var item in prop.EnumerateArray())
        {
            if (list.Count >= maxItems) break;
            if (item.ValueKind != JsonValueKind.String) continue;
            string s = item.GetString() ?? "";
            if (s.Length == 0) continue;
            list.Add(s.Length > maxChars ? s[..maxChars] : s);
        }
        return list;
    }
}
