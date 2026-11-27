using System.Text.Json;

namespace Waybridge.Control.Core;

public sealed record RemoteSessionWindow(uint Id, string Title, string AppId, bool X11);

public sealed record RemoteSessionBackgroundApp(string Id, string AppId, string Title, string Status);

public sealed record RemoteSessionProcess(int Pid, string Name, string AppId, int Count);

public sealed record RemoteSessionOverview(
    string Name,
    int Pid,
    string Started,
    int Port,
    bool? ViewerConnected,
    long? UptimeSeconds,
    IReadOnlyList<RemoteSessionWindow> Windows,
    IReadOnlyList<RemoteSessionBackgroundApp> BackgroundApps,
    IReadOnlyList<RemoteSessionProcess> Processes,
    bool DetailsAvailable);

public static class RemoteSessionModel
{
    public static IReadOnlyList<RemoteSessionOverview> ParseList(JsonElement element)
    {
        if (element.ValueKind != JsonValueKind.Array) return [];
        var sessions = new List<RemoteSessionOverview>();
        foreach (var item in element.EnumerateArray())
        {
            if (TryParseSession(item, out var session) && session is not null)
                sessions.Add(session);
        }
        return sessions;
    }

    public static bool TryParseSession(JsonElement element, out RemoteSessionOverview? session)
    {
        session = null;
        if (element.ValueKind != JsonValueKind.Object) return false;
        string name = GetString(element, "name", 64);
        if (name.Length == 0) return false;
        int pid = GetInt32(element, "pid") ?? 0;
        string started = GetString(element, "started", 64);
        int port = GetInt32(element, "port") ?? 0;
        if (pid <= 0 || started.Length == 0) return false;

        bool? viewerConnected = element.TryGetProperty("viewer_connected", out var viewerProp)
            ? viewerProp.ValueKind switch
            {
                JsonValueKind.True => true,
                JsonValueKind.False => false,
                _ => null
            }
            : null;
        long? uptimeSeconds = element.TryGetProperty("uptime_seconds", out var uptimeProp) &&
            uptimeProp.ValueKind == JsonValueKind.Number && uptimeProp.TryGetInt64(out long seconds) && seconds >= 0
            ? seconds : null;
        bool stateAvailable = element.TryGetProperty("state_available", out var availProp) && availProp.ValueKind == JsonValueKind.True;
        bool hasProcesses = element.TryGetProperty("processes", out var procsProp) && procsProp.ValueKind == JsonValueKind.Array;

        var windows = new List<RemoteSessionWindow>();
        if (element.TryGetProperty("windows", out var windowsProp) && windowsProp.ValueKind == JsonValueKind.Array)
        {
            foreach (var win in windowsProp.EnumerateArray())
            {
                if (windows.Count >= 128 || win.ValueKind != JsonValueKind.Object) continue;
                uint id = win.TryGetProperty("id", out var idProp) && idProp.ValueKind == JsonValueKind.Number && idProp.TryGetUInt32(out uint parsed)
                    ? parsed : 0u;
                string title = GetString(win, "title", 256);
                string appId = GetString(win, "app_id", 255);
                bool x11 = win.TryGetProperty("x11", out var x11Prop) && x11Prop.ValueKind == JsonValueKind.True;
                windows.Add(new(id, title, appId, x11));
            }
        }

        var backgroundApps = new List<RemoteSessionBackgroundApp>();
        if (element.TryGetProperty("background_apps", out var bgProp) && bgProp.ValueKind == JsonValueKind.Array)
        {
            foreach (var bg in bgProp.EnumerateArray())
            {
                if (backgroundApps.Count >= 64 || bg.ValueKind != JsonValueKind.Object) continue;
                string id = GetString(bg, "id", 256);
                string appId = GetString(bg, "app_id", 255);
                string title = GetString(bg, "title", 256);
                string status = GetString(bg, "status", 32);
                backgroundApps.Add(new(id, appId, title, status));
            }
        }

        var processes = new List<RemoteSessionProcess>();
        if (hasProcesses)
        {
            foreach (var proc in procsProp.EnumerateArray())
            {
                if (processes.Count >= 64 || proc.ValueKind != JsonValueKind.Object) continue;
                int procPid = GetInt32(proc, "pid") ?? 0;
                string procName = GetString(proc, "name", 64);
                string procAppId = GetString(proc, "app_id", 255);
                int count = Math.Max(1, GetInt32(proc, "count") ?? 1);
                if (procPid > 0 && procName.Length > 0)
                    processes.Add(new(procPid, procName, procAppId, count));
            }
        }

        session = new RemoteSessionOverview(
            name, pid, started, port, viewerConnected, uptimeSeconds,
            windows, backgroundApps, processes, stateAvailable || hasProcesses);
        return true;
    }

    public static string FormatUptime(long seconds)
    {
        if (seconds < 60) return Localization.Tr("less than 1 min");
        long minutes = seconds / 60;
        if (minutes < 60) return Localization.Tr("{0} min", minutes);
        long hours = minutes / 60;
        long remMinutes = minutes % 60;
        if (hours < 24)
            return remMinutes > 0 ? Localization.Tr("{0} h {1} min", hours, remMinutes) : Localization.Tr("{0} h", hours);
        long days = hours / 24;
        long remHours = hours % 24;
        return remHours > 0 ? Localization.Tr("{0} d {1} h", days, remHours) : Localization.Tr("{0} d", days);
    }

    private static string GetString(JsonElement obj, string name, int limit)
    {
        if (!obj.TryGetProperty(name, out var prop) || prop.ValueKind != JsonValueKind.String) return "";
        string raw = prop.GetString() ?? "";
        return raw.Length > limit ? raw[..limit] : raw;
    }

    private static int? GetInt32(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var prop) && prop.ValueKind == JsonValueKind.Number && prop.TryGetInt32(out int value)
            ? value : null;
}
