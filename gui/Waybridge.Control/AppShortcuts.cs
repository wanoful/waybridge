using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal static class AppShortcuts
{
    private const string ProtocolKey = @"Software\Classes\waybridge";
    private static readonly Guid ShellLinkClass = new("00021401-0000-0000-C000-000000000046");
    private static string Executable => Environment.ProcessPath ?? throw new IOException("无法找到 Waybridge 程序");

    public static void RegisterProtocol()
    {
        string target = $"\"{Executable}\" --launch-link \"%1\"";
        using var protocol = Registry.CurrentUser.CreateSubKey(ProtocolKey);
        using var command = protocol.CreateSubKey(@"shell\open\command");
        if (Equals(command.GetValue(null), target) && Equals(protocol.GetValue("URL Protocol"), "")) return;
        protocol.SetValue(null, "URL:Waybridge 应用链接");
        protocol.SetValue("URL Protocol", "");
        using var icon = protocol.CreateSubKey("DefaultIcon");
        icon.SetValue(null, $"\"{Executable}\",0");
        command.SetValue(null, target);
        SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero); // SHCNE_ASSOCCHANGED
    }

    public static string Save(AppLaunchLink link, string name, string profileName, bool desktop,
                              string stateDirectory, byte[]? png)
    {
        var folder = Environment.GetFolderPath(desktop ? Environment.SpecialFolder.DesktopDirectory : Environment.SpecialFolder.Programs,
            Environment.SpecialFolderOption.Create);
        if (string.IsNullOrEmpty(folder)) throw new IOException("找不到 Windows 快捷方式目录");
        if (!desktop) folder = Path.Combine(folder, "Waybridge");
        Directory.CreateDirectory(folder);

        string arguments = $"--launch-link \"{link}\"";
        string label = $"{FileLabel(name, 72)} ({FileLabel(profileName, 36)})";
        string path = Path.Combine(folder, label + ".lnk");
        if (File.Exists(path) && !IsSameShortcut(path, arguments))
        {
            string suffix = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(link.ToString())))[..12];
            path = Path.Combine(folder, $"{label} - {suffix}.lnk");
            if (File.Exists(path) && !IsSameShortcut(path, arguments))
                throw new IOException("同名快捷方式已存在，请先重命名该快捷方式");
        }
        string? iconPath = null;
        if (png is not null)
        {
            string icons = Path.Combine(stateDirectory, "shortcuts", "icons");
            iconPath = Path.Combine(icons, Convert.ToHexString(SHA256.HashData(png)) + ".ico");
            Directory.CreateDirectory(icons);
            if (!File.Exists(iconPath)) WriteIcon(iconPath, png);
        }

        var shellLink = NewShellLink();
        string temporary = Path.Combine(folder, $".waybridge-{Guid.NewGuid():N}.lnk");
        try
        {
            shellLink.SetPath(Executable);
            shellLink.SetArguments(arguments);
            shellLink.SetWorkingDirectory(AppContext.BaseDirectory);
            shellLink.SetDescription(label);
            shellLink.SetIconLocation(iconPath ?? Executable, 0);
            ((IPersistFile)shellLink).Save(temporary, true);
            File.Move(temporary, path, true);
            return path;
        }
        finally
        {
            Marshal.FinalReleaseComObject(shellLink);
            File.Delete(temporary);
        }
    }

    private static bool IsSameShortcut(string path, string arguments)
    {
        var shortcut = NewShellLink();
        try
        {
            ((IPersistFile)shortcut).Load(path, 0);
            var value = new StringBuilder(AppLaunchLink.MaxLength + 32);
            shortcut.GetArguments(value, value.Capacity);
            return value.ToString() == arguments;
        }
        catch (COMException) { return false; }
        finally { Marshal.FinalReleaseComObject(shortcut); }
    }

    private static IShellLink NewShellLink() =>
        (IShellLink)Activator.CreateInstance(Type.GetTypeFromCLSID(ShellLinkClass, throwOnError: true)!)!;

    private static string FileLabel(string value, int max)
    {
        var illegal = Path.GetInvalidFileNameChars();
        var label = new string(value.Select(c => char.IsControl(c) || illegal.Contains(c) ? ' ' : c)
            .Take(max).ToArray()).Trim(' ', '.');
        if (label.Length > 0 && char.IsHighSurrogate(label[^1])) label = label[..^1];
        return label.Length > 0 ? label : "Waybridge";
    }

    // The launcher cache already validates the 128x128 PNG and its content hash.
    private static void WriteIcon(string path, byte[] png)
    {
        var ico = new byte[22 + png.Length];
        ico[2] = ico[4] = ico[10] = 1;
        ico[6] = ico[7] = 128;
        ico[12] = 32;
        BinaryPrimitives.WriteUInt32LittleEndian(ico.AsSpan(14), (uint)png.Length);
        ico[18] = 22;
        png.CopyTo(ico, 22);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllBytes(temporary, ico);
            File.Move(temporary, path, true);
        }
        finally { File.Delete(temporary); }
    }

    [DllImport("shell32.dll")]
    private static extern void SHChangeNotify(uint eventId, uint flags, IntPtr item1, IntPtr item2);

    [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellLink
    {
        void GetPath([MarshalAs(UnmanagedType.LPWStr)] StringBuilder file, int length, IntPtr data, uint flags);
        void GetIDList(out IntPtr idList);
        void SetIDList(IntPtr idList);
        void GetDescription([MarshalAs(UnmanagedType.LPWStr)] StringBuilder description, int length);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string description);
        void GetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] StringBuilder directory, int length);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string directory);
        void GetArguments([MarshalAs(UnmanagedType.LPWStr)] StringBuilder arguments, int length);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string arguments);
        void GetHotkey(out ushort hotkey);
        void SetHotkey(ushort hotkey);
        void GetShowCmd(out int command);
        void SetShowCmd(int command);
        void GetIconLocation([MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int length, out int index);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int index);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
        void Resolve(IntPtr hwnd, uint flags);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string path);
    }
}
