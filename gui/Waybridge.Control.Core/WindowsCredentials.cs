using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Waybridge.Control.Core;

public sealed class WindowsCredentialStore : ISecretStore
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
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Windows Credential Manager is only available on Windows.");
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
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Windows Credential Manager is only available on Windows.");
        byte[] bytes = Encoding.Unicode.GetBytes(value);
        if (bytes.Length > 2560) throw new ArgumentException(Tr("The token is too long for Windows Credential Manager"));
        IntPtr pointer = Marshal.AllocHGlobal(bytes.Length);
        try
        {
            Marshal.Copy(bytes, 0, pointer, bytes.Length);
            var credential = new Credential
            {
                Type = 1, TargetName = Target(profile), Persist = 2,
                CredentialBlobSize = (uint)bytes.Length, CredentialBlob = pointer, UserName = "Waybridge"
            };
            if (!WriteCredential(ref credential, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        finally { Array.Clear(bytes); Marshal.FreeHGlobal(pointer); }
    }

    public void Delete(Guid profile)
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Windows Credential Manager is only available on Windows.");
        if (!DeleteCredential(Target(profile), 1, 0) && Marshal.GetLastWin32Error() != 1168)
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
