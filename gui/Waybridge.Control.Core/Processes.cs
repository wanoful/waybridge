using System.Diagnostics;
using System.Text;

namespace Waybridge.Control.Core;

internal static class Processes
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
        return Process.Start(info) ?? throw new IOException(Tr("Could not launch ") + Path.GetFileName(executable));
    }

    public static async Task StopAsync(Process? process)
    {
        if (process is null) return;
        try { if (!process.HasExited) process.Kill(true); await process.WaitForExitAsync(); }
        catch (InvalidOperationException) { }
        finally { process.Dispose(); }
    }
}
