using System.Runtime.InteropServices;

namespace Waybridge.Control;

// Only BCL/Win32 calls here: startup logging must not depend on WinUI activation.
internal static class StartupDiagnostics
{
    private static readonly object gate = new();
    private static bool reporting;
    public static string LogPath { get; } = Environment.GetEnvironmentVariable("WAYBRIDGE_STARTUP_LOG")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Waybridge", "startup.log");
    public static void Install()
    {
        Write($"--- startup {Environment.ProcessId} | app {typeof(StartupDiagnostics).Assembly.GetName().Version} | process {RuntimeInformation.ProcessArchitecture} | OS {RuntimeInformation.OSArchitecture} | {RuntimeInformation.OSDescription} | {RuntimeInformation.FrameworkDescription} ---");
        Write("base directory: " + AppContext.BaseDirectory);
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception error) Fatal("AppDomain unhandled exception", error);
        };
    }
    public static void Write(string message)
    {
        try
        {
            lock (gate)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
                if (File.Exists(LogPath) && new FileInfo(LogPath).Length > 1024 * 1024)
                    File.Move(LogPath, LogPath + ".previous", true);
                File.AppendAllText(LogPath, DateTimeOffset.Now.ToString("O") + " " + message + Environment.NewLine);
            }
        }
        catch { /* Diagnostics must not introduce a second startup failure. */ }
    }
    public static void Fatal(string phase, Exception error)
    {
        Write($"FATAL {phase}: HRESULT 0x{error.HResult:X8}\n{error}");
        lock (gate) { if (reporting) return; reporting = true; }
        try
        {
            MessageBoxW(IntPtr.Zero, $"启动失败（{phase}）\n\n{error.GetType().Name}: {error.Message}\nHRESULT: 0x{error.HResult:X8}\n\n日志：{LogPath}\n\n请运行目录中的 Diagnose-Startup.cmd 收集完整报告。", "Waybridge 启动失败", 0x10);
        }
        catch { }
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int MessageBoxW(IntPtr owner, string text, string title, uint type);
}
