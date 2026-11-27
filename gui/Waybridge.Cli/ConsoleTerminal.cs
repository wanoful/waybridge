using System.Runtime.InteropServices;

namespace Waybridge.Cli;

internal sealed class ConsoleTerminal : IDisposable
{
    private const int StdInputHandle = -10;
    private const int StdOutputHandle = -11;
    private const uint EnableProcessedInput = 0x0001;
    private const uint EnableLineInput = 0x0002;
    private const uint EnableEchoInput = 0x0004;
    private const uint EnableVirtualTerminalInput = 0x0200;
    private const uint EnableProcessedOutput = 0x0001;
    private const uint EnableVirtualTerminalProcessing = 0x0004;
    private const uint DisableNewlineAutoReturn = 0x0008;
    private const uint CtrlCEvent = 0;
    private const uint CtrlBreakEvent = 1;
    private const uint Utf8CodePage = 65001;

    private delegate bool ConsoleCtrlHandler(uint ctrlType);

    [StructLayout(LayoutKind.Sequential)]
    private struct Coord { public short X, Y; }

    [StructLayout(LayoutKind.Sequential)]
    private struct SmallRect { public short Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct ConsoleScreenBufferInfo
    {
        public Coord Size;
        public Coord CursorPosition;
        public ushort Attributes;
        public SmallRect Window;
        public Coord MaximumWindowSize;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GetStdHandle(int nStdHandle);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetConsoleMode(IntPtr handle, out uint mode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleMode(IntPtr handle, uint mode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetConsoleScreenBufferInfo(IntPtr handle, out ConsoleScreenBufferInfo info);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool ReadFile(IntPtr handle, [Out] byte[] buffer, uint toRead, out uint read, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(IntPtr handle, byte[] buffer, uint toWrite, out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CancelIoEx(IntPtr handle, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetConsoleCP();
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint GetConsoleOutputCP();
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleCP(uint codePage);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleOutputCP(uint codePage);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetConsoleCtrlHandler(ConsoleCtrlHandler handler, bool add);

    private readonly bool windowsConsole;
    private readonly IntPtr stdinHandle;
    private readonly IntPtr stdoutHandle;
    private readonly uint originalInputMode;
    private readonly uint originalOutputMode;
    private readonly uint originalInputCp;
    private readonly uint originalOutputCp;
    private readonly ConsoleCtrlHandler? ctrlHandler;
    private readonly Stream? fallbackInput;
    private readonly Stream? fallbackOutput;
    private bool disposed;

    public bool IsInteractiveConsole => windowsConsole;

    private ConsoleTerminal(
        bool windowsConsole,
        IntPtr stdinHandle,
        IntPtr stdoutHandle,
        uint originalInputMode,
        uint originalOutputMode,
        uint originalInputCp,
        uint originalOutputCp,
        ConsoleCtrlHandler? ctrlHandler,
        Stream? fallbackInput,
        Stream? fallbackOutput)
    {
        this.windowsConsole = windowsConsole;
        this.stdinHandle = stdinHandle;
        this.stdoutHandle = stdoutHandle;
        this.originalInputMode = originalInputMode;
        this.originalOutputMode = originalOutputMode;
        this.originalInputCp = originalInputCp;
        this.originalOutputCp = originalOutputCp;
        this.ctrlHandler = ctrlHandler;
        this.fallbackInput = fallbackInput;
        this.fallbackOutput = fallbackOutput;
    }

    public static ConsoleTerminal Enter(Action onExternalClose)
    {
        if (OperatingSystem.IsWindows())
        {
            IntPtr hIn = GetStdHandle(StdInputHandle);
            IntPtr hOut = GetStdHandle(StdOutputHandle);
            if (hIn != IntPtr.Zero && hIn != new IntPtr(-1) &&
                hOut != IntPtr.Zero && hOut != new IntPtr(-1) &&
                GetConsoleMode(hIn, out uint inMode) &&
                GetConsoleMode(hOut, out uint outMode))
            {
                uint inCp = GetConsoleCP();
                uint outCp = GetConsoleOutputCP();
                SetConsoleCP(Utf8CodePage);
                SetConsoleOutputCP(Utf8CodePage);
                uint rawIn = (inMode & ~(EnableEchoInput | EnableLineInput | EnableProcessedInput)) | EnableVirtualTerminalInput;
                uint rawOut = outMode | EnableProcessedOutput | EnableVirtualTerminalProcessing | DisableNewlineAutoReturn;
                SetConsoleMode(hIn, rawIn);
                SetConsoleMode(hOut, rawOut);
                ConsoleCtrlHandler handler = ctrlType =>
                {
                    if (ctrlType is CtrlCEvent or CtrlBreakEvent) return true;
                    onExternalClose();
                    return true;
                };
                SetConsoleCtrlHandler(handler, true);
                return new ConsoleTerminal(true, hIn, hOut, inMode, outMode, inCp, outCp, handler, null, null);
            }
        }
        return new ConsoleTerminal(
            false, IntPtr.Zero, IntPtr.Zero, 0, 0, 0, 0, null,
            Console.OpenStandardInput(), Console.OpenStandardOutput());
    }

    public (ushort Cols, ushort Rows) GetSize()
    {
        if (windowsConsole && GetConsoleScreenBufferInfo(stdoutHandle, out var info))
        {
            int cols = Math.Clamp(info.Window.Right - info.Window.Left + 1, 1, 1000);
            int rows = Math.Clamp(info.Window.Bottom - info.Window.Top + 1, 1, 500);
            return ((ushort)cols, (ushort)rows);
        }
        try
        {
            int cols = Math.Clamp(Console.WindowWidth, 1, 1000);
            int rows = Math.Clamp(Console.WindowHeight, 1, 500);
            if (cols > 0 && rows > 0) return ((ushort)cols, (ushort)rows);
        }
        catch { }
        return (120, 30);
    }

    public Task<int> ReadInputAsync(byte[] buffer, CancellationToken cancellationToken)
    {
        if (!windowsConsole)
            return fallbackInput!.ReadAsync(buffer, 0, buffer.Length, cancellationToken);

        return Task.Run(() =>
        {
            using var cancelReg = cancellationToken.Register(() => CancelIoEx(stdinHandle, IntPtr.Zero));
            if (cancellationToken.IsCancellationRequested) return 0;
            if (!ReadFile(stdinHandle, buffer, (uint)buffer.Length, out uint read, IntPtr.Zero))
            {
                if (cancellationToken.IsCancellationRequested) return 0;
                return 0;
            }
            return (int)read;
        }, cancellationToken);
    }

    public void WriteOutput(byte[] data)
    {
        if (data.Length == 0) return;
        if (windowsConsole)
        {
            int offset = 0;
            while (offset < data.Length)
            {
                byte[] chunk = offset == 0 ? data : data[offset..];
                if (!WriteFile(stdoutHandle, chunk, (uint)chunk.Length, out uint written, IntPtr.Zero) || written == 0)
                    break;
                offset += (int)written;
            }
            return;
        }
        fallbackOutput!.Write(data, 0, data.Length);
        fallbackOutput.Flush();
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        if (windowsConsole)
        {
            if (ctrlHandler is not null) SetConsoleCtrlHandler(ctrlHandler, false);
            SetConsoleMode(stdinHandle, originalInputMode);
            SetConsoleMode(stdoutHandle, originalOutputMode);
            if (originalInputCp != 0) SetConsoleCP(originalInputCp);
            if (originalOutputCp != 0) SetConsoleOutputCP(originalOutputCp);
        }
        fallbackInput?.Dispose();
        fallbackOutput?.Dispose();
    }
}
