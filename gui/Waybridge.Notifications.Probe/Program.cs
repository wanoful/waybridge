using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Xml.Linq;
using Microsoft.Win32;
using Microsoft.Windows.AppNotifications;
using Waybridge.Control;

// Isolated real-Windows probe: no profile/credential store or real Waybridge
// app identity is used. It touches only this unique registration's toasts.
internal static class Program
{
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SetCurrentProcessExplicitAppUserModelID(string id);
    [ComImport, Guid("53E31837-6600-4A81-9395-75CFFE746F94"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface INotificationActivationCallback
    {
        void Activate([MarshalAs(UnmanagedType.LPWStr)] string appId,
            [MarshalAs(UnmanagedType.LPWStr)] string arguments, IntPtr input, uint count);
    }
    private static void Activate(string appId, string arguments)
    {
        var iid = typeof(INotificationActivationCallback).GUID;
        // Call the actual native callback interface on this registered manager.
        // This avoids asking COM to cold-launch a second copy of a test runner.
        var native = ((WinRT.IWinRTObject)AppNotificationManager.Default).NativeObject;
        Marshal.ThrowExceptionForHR(Marshal.QueryInterface(native.ThisPtr, in iid, out IntPtr pointer));
        try { ((INotificationActivationCallback)Marshal.GetObjectForIUnknown(pointer)).Activate(appId, arguments, IntPtr.Zero, 0); }
        finally { Marshal.Release(pointer); }
    }
    private static async Task Wait(Func<Task<bool>> predicate, string step)
    {
        var deadline = DateTime.UtcNow.AddSeconds(20);
        while (DateTime.UtcNow < deadline) { if (await predicate()) return; await Task.Delay(50); }
        throw new Exception("probe timeout: " + step);
    }
    private static JsonElement Message(uint id, ulong revision, string type = "notification", int timeout = 30000) => JsonSerializer.SerializeToElement(new
    {
        type, instance = new string('a',32), binding = new string('b',32), id, revision,
        app_name = "Waybridge 独立通知测试", summary = "中文通知验收", body = "临时探针消息",
        actions = new[] { new { key = "default", label = "打开" }, new { key = "accept", label = "确定" } },
        timeout_ms = timeout, png = (string?)null
    });
    [MTAThread]
    private static async Task<int> Main(string[] args)
    {
        if (args.Any(a => a.Contains("AppNotificationActivated", StringComparison.Ordinal))) return 0;
        string appId = "Waybridge.Notifications.Probe." + Guid.NewGuid().ToString("N");
        if (args.Length == 2 && args[0] == "--cleanup-id" && args[1].StartsWith("Waybridge.Notifications.Probe.", StringComparison.Ordinal)) appId = args[1];
        SetCurrentProcessExplicitAppUserModelID(appId);
        WinRT.ComWrappersSupport.InitializeComWrappers();
        if (args.Length == 2 && args[0] == "--cleanup-id")
        {
            try { await AppNotificationManager.Default.RemoveAllAsync(); AppNotificationManager.Default.UnregisterAll(); return 0; } catch { return 1; }
        }
        var events = new ConcurrentQueue<JsonElement>();
        var warnings = new ConcurrentQueue<string>();
        DesktopNotifications? bridge = null;
        try
        {
            bridge = new DesktopNotifications(value => { events.Enqueue(JsonSerializer.SerializeToElement(value)); return Task.CompletedTask; }, warnings.Enqueue);
            bridge.ConnectionChanged(true);
            await Wait(() => Task.FromResult(events.Any(e => e.GetProperty("type").GetString() == "notification_available" && e.GetProperty("available").GetBoolean())), "native notifications available (run as interactive non-elevated user)");
            var manager = AppNotificationManager.Default;
            Console.WriteLine("STEP: native notifications available");
            bridge.Receive(Message(17,1));
            await Wait(async () => (await manager.GetAllAsync()).Count(n => n.Tag == "17") == 1, "Show/GetAll");
            var old = (await manager.GetAllAsync()).Single(n => n.Tag == "17");
            Console.WriteLine("STEP: Show/GetAll confirmed");
            string oldArguments = XElement.Parse(old.Payload).Descendants("action").First().Attribute("arguments")!.Value;
            bridge.Receive(Message(17,2));
            await Wait(async () => { var all = await manager.GetAllAsync(); return all.Count(n => n.Tag == "17") == 1 && all.Single(n => n.Tag == "17").Id != old.Id; }, "atomic Tag/Group replacement");
            var current = (await manager.GetAllAsync()).Single(n => n.Tag == "17");
            Console.WriteLine("STEP: atomic replacement confirmed");
            string arguments = XElement.Parse(current.Payload).Descendants("action").First().Attribute("arguments")!.Value;
            // Native COM callback → AppNotificationManager.NotificationInvoked →
            // production helper → bound engine IPC feedback. This exercises the
            // actual Windows activation path, without clicking unrelated UI.
            Activate(appId, oldArguments); await Task.Delay(200);
            if (events.Any(e => e.GetProperty("type").GetString() == "notification_feedback")) throw new Exception("stale replacement action accepted");
            Activate(appId, arguments);
            await Wait(() => Task.FromResult(events.Any(e => e.GetProperty("type").GetString() == "notification_feedback" && e.GetProperty("revision").GetUInt64() == 2 && e.GetProperty("action").GetString() == "accept")), "native action callback/IPC");
            await Wait(async () => !(await manager.GetAllAsync()).Any(n => n.Tag == "17"), "action removal");
            bridge.Receive(Message(18,3));
            await Wait(async () => (await manager.GetAllAsync()).Any(n => n.Tag == "18"), "second show");
            bridge.Receive(Message(18,3,"notification_close"));
            await Wait(async () => !(await manager.GetAllAsync()).Any(n => n.Tag == "18"), "application CloseNotification");
            bridge.Receive(Message(19,4));
            await Wait(async () => (await manager.GetAllAsync()).Any(n => n.Tag == "19"), "dismissal show");
            var dismissed=(await manager.GetAllAsync()).Single(n => n.Tag == "19"); await manager.RemoveByIdAsync(dismissed.Id);
            await Wait(() => Task.FromResult(events.Any(e => e.GetProperty("type").GetString() == "notification_feedback" && e.GetProperty("id").GetUInt32() == 19 && e.GetProperty("reason").GetUInt32() == 2)), "notification-center dismissal");
            bridge.Receive(Message(20,5));
            await Wait(async () => (await manager.GetAllAsync()).Any(n => n.Tag == "20"), "disconnect show");
            bridge.ConnectionChanged(false);
            await Wait(async () => !(await manager.GetAllAsync()).Any(), "disconnect cleanup");
            Console.WriteLine("PASS: real Windows toast Show/GetAll, single Tag/Group replacement, native COM activation and stale-action rejection, application close, dismissal feedback, disconnect cleanup");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("FAIL: " + error.GetType().Name + ": " + error.Message);
            foreach (var warning in warnings) Console.Error.WriteLine(warning);
            return 1;
        }
        finally
        {
            // UnregisterAll must run while the probe still owns its native COM
            // registration; Unregister first can make some 1.8 builds return
            // ERROR_NOT_FOUND before cleaning their per-AppID registry assets.
            try { AppNotificationManager.Default.UnregisterAll(); } catch { }
            if (bridge is not null) await bridge.DisposeAsync();
        }
    }
}
