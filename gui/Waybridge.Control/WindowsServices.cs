using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed class CredentialStore : ISecretStore
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Credential
    {
        public uint Flags, Type;
        public string? TargetName, Comment;
        public long LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist, AttributeCount;
        public IntPtr Attributes;
        public string? TargetAlias, UserName;
    }
    [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ReadCredential(string target, uint type, uint flags, out IntPtr result);
    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool WriteCredential(ref Credential credential, uint flags);
    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool DeleteCredential(string target, uint type, uint flags);
    [DllImport("advapi32.dll")] private static extern void CredFree(IntPtr pointer);
    private static string Target(Guid id) => "Waybridge/session/" + id.ToString("N");
    public string? Read(Guid profile)
    {
        if (!ReadCredential(Target(profile), 1, 0, out var pointer))
        {
            int error = Marshal.GetLastWin32Error();
            if (error == 1168) return null;
            throw new Win32Exception(error);
        }
        try
        {
            var value = Marshal.PtrToStructure<Credential>(pointer);
            return Marshal.PtrToStringUni(value.CredentialBlob, (int)value.CredentialBlobSize / 2);
        }
        finally { CredFree(pointer); }
    }
    public void Write(Guid profile, string value)
    {
        byte[] bytes = Encoding.Unicode.GetBytes(value);
        if (bytes.Length > 2560) throw new ArgumentException(Tr("The token is too long for Windows Credential Manager"));
        IntPtr pointer = Marshal.AllocHGlobal(bytes.Length);
        try
        {
            Marshal.Copy(bytes, 0, pointer, bytes.Length);
            var credential = new Credential { Type = 1, TargetName = Target(profile), Persist = 2,
                CredentialBlobSize = (uint)bytes.Length, CredentialBlob = pointer, UserName = "Waybridge" };
            if (!WriteCredential(ref credential, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        finally { Array.Clear(bytes); Marshal.FreeHGlobal(pointer); }
    }
    public void Delete(Guid profile)
    {
        if (!DeleteCredential(Target(profile), 1, 0) && Marshal.GetLastWin32Error() != 1168)
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}

internal sealed class TrayIcon : IDisposable
{
    private const uint Callback = 0x8000 + 37, ShowMessage = 0x8000 + 38, CopyDataMessage = 0x004a;
    private const uint LaunchLinkData = 0x57424c4b;
    private readonly WndProc callback;
    private readonly IntPtr hwnd;
    private readonly Action<string> action;
    private readonly Action<AppLaunchLink>? launchLink;
    private bool connected;
    private const string ClassName = "WaybridgeControlCenterTray";
    private delegate IntPtr WndProc(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass
    {
        public uint Style; public IntPtr Procedure; public int ClassExtra, WindowExtra;
        public IntPtr Instance, Icon, Cursor, Background; public string? MenuName, ClassName;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyData
    {
        public uint Size; public IntPtr Window; public uint Id, Flags, Message; public IntPtr Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Tip;
        public uint State, StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Info;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string InfoTitle;
        public uint InfoFlags; public Guid Guid; public IntPtr BalloonIcon;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct CopyData { public nuint Kind; public uint Bytes; public IntPtr Value; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern ushort RegisterClassW(ref WindowClass wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CreateWindowExW(uint ex, string name, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] private static extern IntPtr DefWindowProcW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr LoadImageW(IntPtr instance, string name, uint type, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(IntPtr icon);
    [DllImport("user32.dll")] private static extern IntPtr CreatePopupMenu();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool AppendMenuW(IntPtr menu, uint flags, nuint id, string text);
    [DllImport("user32.dll")] private static extern uint TrackPopupMenu(IntPtr menu, uint flags, int x, int y, int reserved, IntPtr owner, IntPtr rect);
    [DllImport("user32.dll")] private static extern bool DestroyMenu(IntPtr menu);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern bool Shell_NotifyIconW(uint message, ref NotifyData data);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandleW(string? name);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string name, string? title);
    [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint message, IntPtr wp,
        ref CopyData data, uint flags, uint timeout, out nuint result);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] private static extern bool AllowSetForegroundWindow(uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] private static extern int MessageBoxW(
        IntPtr owner, string text, string title, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessageW(string name);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    private NotifyData data;
    private readonly uint taskbarCreated = RegisterWindowMessageW("TaskbarCreated");
    public TrayIcon(Action<string> action, Action<AppLaunchLink>? launchLink = null)
    {
        this.action = action; this.launchLink = launchLink; callback = Procedure;
        var wc = new WindowClass { ClassName = ClassName, Instance = GetModuleHandleW(null),
            Procedure = Marshal.GetFunctionPointerForDelegate(callback) };
        RegisterClassW(ref wc);
        hwnd = CreateWindowExW(0x80, ClassName, "", 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, wc.Instance, IntPtr.Zero);
        if (hwnd == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        data = new NotifyData { Size = (uint)Marshal.SizeOf<NotifyData>(), Window = hwnd, Id = 1,
            Flags = 1 | 2 | 4, Message = Callback, Icon = LoadImageW(IntPtr.Zero, Path.Combine(AppContext.BaseDirectory, "Assets", "waybridge.ico"), 1, 0, 0, 0x10 | 0x40),
            Tip = Tr("Waybridge · Disconnected"), Info = "", InfoTitle = "" };
        if (!Shell_NotifyIconW(0, ref data)) throw new IOException(Tr("Could not create the system tray icon"));
    }
    public static void ShowExisting()
    {
        var existing = FindWindowExW(IntPtr.Zero, IntPtr.Zero, ClassName, null);
        if (existing != IntPtr.Zero) PostMessageW(existing, ShowMessage, IntPtr.Zero, IntPtr.Zero);
    }
    public static bool ForwardLaunchLink(AppLaunchLink link)
    {
        var deadline = Environment.TickCount64 + 5000;
        IntPtr existing;
        while ((existing = FindWindowExW(IntPtr.Zero, IntPtr.Zero, ClassName, null)) == IntPtr.Zero)
        {
            if (Environment.TickCount64 >= deadline) return false;
            Thread.Sleep(50);
        }
        string url = link.ToString();
        IntPtr buffer = Marshal.StringToHGlobalUni(url);
        try
        {
            var data = new CopyData { Kind = LaunchLinkData, Bytes = (uint)(url.Length * sizeof(char)), Value = buffer };
            GetWindowThreadProcessId(existing, out uint owner);
            if (owner != 0) AllowSetForegroundWindow(owner);
            return SendMessageTimeoutW(existing, CopyDataMessage, IntPtr.Zero, ref data, 0x3, 1500, out nuint result)
                != IntPtr.Zero && result == 1;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
    public static void ShowLinkError(string message) => MessageBoxW(IntPtr.Zero, message, Tr("Waybridge app link"), 0x30);
    public static bool ConfirmLinkSwitch(string current, string target) =>
        MessageBoxW(IntPtr.Zero, Tr("Switch to “{0}” to launch this app?\n\nThis disconnects from “{1}” and leaves its apps running.", target, current),
            Tr("Switch Waybridge workstation"), 0x10124) == 6; // Yes/No, with No as the default.
    public void Update(bool value)
    {
        connected = value; data.Tip = value ? Tr("Waybridge · Connected") : Tr("Waybridge · Disconnected");
        Shell_NotifyIconW(1, ref data);
    }
    private IntPtr Procedure(IntPtr window, uint message, IntPtr wp, IntPtr lp)
    {
        if (message == taskbarCreated) { Shell_NotifyIconW(0, ref data); return IntPtr.Zero; }
        if (message == ShowMessage) { action("show"); return IntPtr.Zero; }
        if (message == CopyDataMessage && launchLink is not null && lp != IntPtr.Zero)
        {
            var incoming = Marshal.PtrToStructure<CopyData>(lp);
            if (incoming.Kind == LaunchLinkData && incoming.Bytes is > 0 and <= AppLaunchLink.MaxLength * sizeof(char) &&
                incoming.Bytes % sizeof(char) == 0 && incoming.Value != IntPtr.Zero &&
                Marshal.PtrToStringUni(incoming.Value, (int)incoming.Bytes / sizeof(char)) is { } url &&
                AppLaunchLink.TryParse(url, out var link))
            {
                launchLink(link);
                return new IntPtr(1);
            }
            return IntPtr.Zero;
        }
        if (message == Callback)
        {
            uint evt = unchecked((uint)lp.ToInt64()) & 0xffff;
            if (evt == 0x203 || evt == 0x202) action("show");
            else if (evt is 0x205 or 0x7b)
            {
                var menu = CreatePopupMenu();
                try
                {
                    AppendMenuW(menu, 0, 1, Tr("Open control center"));
                    AppendMenuW(menu, 0, 2, connected ? Tr("Disconnect (keep apps running)") : Tr("Connect"));
                    AppendMenuW(menu, 0, 3, Tr("Toggle mute"));
                    AppendMenuW(menu, 0, 4, Tr("Diagnostics"));
                    AppendMenuW(menu, 0x800, 0, "");
                    AppendMenuW(menu, 0, 5, Tr("Exit client"));
                    GetCursorPos(out var point); SetForegroundWindow(window);
                    uint choice = TrackPopupMenu(menu, 0x100 | 2, point.X, point.Y, 0, window, IntPtr.Zero);
                    if (choice is >= 1 and <= 5) action(new[] { "", "show", "connect", "mute", "diagnostics", "quit" }[choice]);
                    PostMessageW(window, 0, IntPtr.Zero, IntPtr.Zero);
                }
                finally { DestroyMenu(menu); }
            }
            return IntPtr.Zero;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    public void Dispose() { Shell_NotifyIconW(2, ref data); DestroyWindow(hwnd); if (data.Icon != IntPtr.Zero) DestroyIcon(data.Icon); GC.KeepAlive(callback); }
}
