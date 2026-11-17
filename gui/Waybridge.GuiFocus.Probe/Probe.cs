// This project compiles the actual Control UI sources. Only its executable
// identity, entry point and state directory differ from the installed UI.
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.Windows.AppNotifications;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Waybridge.Control.Core;

namespace Waybridge.Control;
internal static class GuiFocusProbe
{
    private static string root = "";
    private static IntPtr originalForeground;
    private static POINT originalCursor;
    private static readonly BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    private static object? Field(object value, string name) => value.GetType().GetField(name, Private)!.GetValue(value);
    private static T Get<T>(object value, string name) => (T)Field(value, name)!;
    private static void Log(string value) => File.AppendAllText(Path.Combine(root, "result.txt"), value + "\n");
    private static void Check(bool value, string message) { if (!value) throw new IOException(message); }
    [STAThread]
    public static void Main(string[] args)
    {
        if (args.Length != 1) { Environment.ExitCode = 2; return; }
        root = Path.GetFullPath(args[0]);
        Check(Path.GetFileName(root).StartsWith("Waybridge-gui-focus-probe-", StringComparison.Ordinal), "own temporary fixture required");
        Environment.SetEnvironmentVariable("WAYBRIDGE_STARTUP_LOG", Path.Combine(root, "startup.log"));
        StartupDiagnostics.Install();
        originalForeground = GetForegroundWindow(); GetCursorPos(out originalCursor);
        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(_ =>
        {
            var queue = DispatcherQueue.GetForCurrentThread();
            SynchronizationContext.SetSynchronizationContext(new DispatcherQueueSynchronizationContext(queue));
            var app = new ControlApplication(Path.Combine(root, "state"));
            queue.TryEnqueue(async () => await RunAsync(app));
        });
    }
    private static async Task Wait(Func<bool> predicate, string label, int timeout = 15000)
    {
        var deadline = Stopwatch.StartNew();
        while (!predicate()) { if (deadline.ElapsedMilliseconds > timeout) throw new TimeoutException(label); await Task.Delay(80); }
    }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject node)
    {
        yield return node;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); ++i)
            foreach (var child in Descendants(VisualTreeHelper.GetChild(node, i))) yield return child;
    }
    private static async Task Click(MainWindow window, Button button)
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
        ShowWindow(hwnd, 9); window.Activate();
        SetWindowPos(hwnd, IntPtr.Zero, 0, 0, 0, 0, 0x13);
        // All injected clicks are checked against this probe's exact UI HWND.
        await Wait(() => button.IsLoaded && button.IsEnabled && button.ActualWidth > 0, "button not ready");
        button.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
        await Task.Delay(500);
        var position = button.TransformToVisual((UIElement)window.Content).TransformPoint(new Windows.Foundation.Point(button.ActualWidth / 2, button.ActualHeight / 2));
        double scale = GetDpiForWindow(hwnd) / 96.0;
        POINT point = new() { x = (int)Math.Round(position.X * scale), y = (int)Math.Round(position.Y * scale) };
        ClientToScreen(hwnd, ref point);
        // Isolate this test's input from an unrelated always-on-top terminal.
        // Revert immediately after the GUI click. Remote product HWNDs are
        // never made topmost: their foreground transfer is what we assert.
        SetWindowPos(hwnd, new IntPtr(-1), 0, 0, 0, 0, 0x13);
        try
        {
            await Task.Delay(150);
            var hit = GetAncestor(WindowFromPoint(point), 2);
            Log($"CLICK {button.Content} point={point.x},{point.y} own={hwnd} hit={hit}");
            Check(hit == hwnd, "refuse click outside own GUI button");
            bool clicked = false;
            RoutedEventHandler observed = (_, _) => { clicked = true; Log("OBSERVED actual button Click"); };
            button.Click += observed;
            SetCursorPos(point.x, point.y);
            try
            {
                Send([Mouse(2), Mouse(4)]);
                await Wait(() => clicked, "real button Click did not fire", 2500);
            }
            finally { button.Click -= observed; }
        }
        finally { SetWindowPos(hwnd, new IntPtr(-2), 0, 0, 0, 0, 0x13); }
    }
    private static INPUT Mouse(uint flags) => new() { type = 0, input = new UNION { mouse = new MOUSEINPUT { flags = flags } } };
    private static INPUT Key(ushort key, bool up = false) => new() { type = 1, input = new UNION { keyboard = new KEYBDINPUT { key = key, flags = up ? 2u : 0 } } };
    private static void Send(INPUT[] inputs) => Check(SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>()) == inputs.Length, "SendInput failed");
    private static string Title(IntPtr hwnd) { var b = new System.Text.StringBuilder(512); GetWindowText(hwnd, b, b.Capacity); return b.ToString(); }
    private static bool Remote(IntPtr hwnd, uint worker, string prefix)
    {
        GetWindowThreadProcessId(hwnd, out uint pid);
        var name = new System.Text.StringBuilder(128); GetClassName(hwnd, name, name.Capacity);
        return pid == worker && name.ToString() == "WaybridgeRemote" && Title(hwnd).StartsWith(prefix, StringComparison.Ordinal);
    }
    private static ConnectionProfile ReadSavedProfile()
    {
        using var stream = new FileStream(Path.Combine(root, "state", "profiles.json"),
            FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        return JsonSerializer.Deserialize<List<ConnectionProfile>>(stream, ProfileStore.Json)!.Single();
    }
    private static void CheckIndependentPolicies(ConnectionProfile profile)
    {
        Check(!profile.Settings.Ime && profile.Settings.ReadOnly && !profile.Settings.Files &&
            !profile.Settings.DragText && profile.Settings.Clipboard == "off",
            "native touch setting changed an independent input/sharing preference");
    }
    private static void Receive(MainWindow window, object message) =>
        typeof(MainWindow).GetMethod("Receive", Private)!.Invoke(window, [JsonSerializer.SerializeToElement(message)]);
    private static object TouchStatus(bool enabled, bool supported, bool live) => new {
        type = "status", uptime_ms = 1UL, tcp_bytes = 0UL, presented_updates = 0UL,
        windows = Array.Empty<object>(), touch_enabled = enabled,
        touch_supported = supported, touch_live_supported = live };
    private static async Task RunSettingsAsync(MainWindow window)
    {
        var navigation = Get<NavigationView>(window, "navigation");
        navigation.SelectedItem = navigation.MenuItems.Cast<NavigationViewItem>().Single(x => x.Tag as string == "input");
        var touch = Get<ToggleSwitch>(window, "touch");
        await Wait(() => touch.IsLoaded && touch.IsEnabled && touch.ActualWidth > 0, "native touch toggle not rendered");
        touch.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
        await Task.Delay(250);
        Check(AutomationProperties.GetAutomationId(touch) == "NativeTouchToggle" &&
            AutomationProperties.GetName(touch).Contains("多点触控", StringComparison.Ordinal),
            "native touch setting lacks its accessible identity");
        Check(!touch.IsOn && !ReadSavedProfile().Settings.Touch, "legacy/default profile unexpectedly enabled native touch");
        CheckIndependentPolicies(ReadSavedProfile());
        Check(!Get<SessionController>(window, "controller").IsRunning, "offline settings probe must not connect");
        var save = Descendants((DependencyObject)window.Content).OfType<Button>().Single(x =>
            AutomationProperties.GetAutomationId(x) == "InputSettingsSave");
        var togglePeer = (IToggleProvider)new ToggleSwitchAutomationPeer(touch).GetPattern(PatternInterface.Toggle)!;
        var savePeer = (IInvokeProvider)new ButtonAutomationPeer(save).GetPattern(PatternInterface.Invoke)!;
        togglePeer.Toggle();
        Check(touch.IsOn, "actual WinUI toggle action did not enable native touch");
        savePeer.Invoke();
        await Wait(() => ReadSavedProfile().Settings.Touch, "enabled touch was not saved by the GUI");
        var enabled = ReadSavedProfile();
        CheckIndependentPolicies(enabled);
        var effective = Get<TextBlock>(window, "touchStatus");
        Check(AutomationProperties.GetAutomationId(effective) == "NativeTouchStatus", "missing persistent touch status identity");
        Receive(window, new { type = "local_settings_applied", settings = new { touch = true, read_only = true },
            touch_enabled = false, touch_live_supported = false });
        Check(touch.IsOn && effective.Text.Contains("当前连接原生触控已关闭", StringComparison.Ordinal) &&
            effective.Text.Contains("重连后生效", StringComparison.Ordinal), "saved touch-on masked old connection's effective off state");
        Receive(window, new { type = "settings_warning", message = "触控设置已保存；当前连接需要重连后生效" });
        Receive(window, new { type = "stream_config", ok = true, fps = 30, bitrate = 12000,
            codec = "png", read_only = true });
        Check(effective.Text.Contains("重连后生效", StringComparison.Ordinal), "generic stream success erased the touch warning");
        savePeer.Invoke();
        await Wait(() => Get<InfoBar>(window, "notice").Message == "连接设置已保存。", "GUI save did not complete");
        Check(effective.Text.Contains("重连后生效", StringComparison.Ordinal), "saved notice erased the touch warning");
        effective.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
        await Wait(() => effective.IsLoaded && effective.ActualWidth > 0, "persistent touch status not rendered");
        Receive(window, TouchStatus(false, false, false));
        Check(effective.Text.Contains("Linux 端", StringComparison.Ordinal) && effective.Text.Contains("重连", StringComparison.Ordinal),
            "unsupported server status did not explain inactive native touch");
        Receive(window, TouchStatus(true, true, true));
        Check(effective.Text.Contains("当前连接原生触控已开启", StringComparison.Ordinal) &&
            effective.Text.Contains("只读模式已暂停", StringComparison.Ordinal) && !effective.Text.Contains("重连", StringComparison.Ordinal),
            "negotiated touch preference was confused with readonly input suspension");
        Receive(window, new { type = "local_settings_applied", settings = new { touch = true, read_only = false },
            touch_enabled = true, touch_live_supported = true });
        Check(!effective.Text.Contains("只读模式已暂停", StringComparison.Ordinal), "readonly touch pause survived a live readonly-off acknowledgement");
        var settingsController = Get<SessionController>(window, "controller");
        var activeField = typeof(SessionController).GetField("active", Private)!;
        var savedActive = activeField.GetValue(settingsController);
        try
        {
            activeField.SetValue(settingsController, ReadSavedProfile());
            Receive(window, new { type = "status", uptime_ms = 2UL, tcp_bytes = 0UL, presented_updates = 0UL,
                windows = new[] { new { id = 17u, app_id = "org.waybridge.PrivateEditor", title = "Private editor",
                    width = 400, height = 300, codec = "PNG", hardware_decode = false, presentation = "GDI" } } });
            var application = Get<ComboBox>(window, "touchApplication");
            var applicationMode = Get<ComboBox>(window, "touchAppMode");
            Check(application.Items.Count == 1 && applicationMode.IsEnabled,
                "active application status did not populate the touch mode selector");
            applicationMode.SelectedIndex = 2;
            savePeer.Invoke();
            await Wait(() => ReadSavedProfile().Settings.TouchModes.GetValueOrDefault("org.waybridge.PrivateEditor") == "mouse",
                "application mouse compatibility mode was not saved");
            var appProfile = ReadSavedProfile(); CheckIndependentPolicies(appProfile);
            typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [appProfile]);
            Check(applicationMode.SelectedIndex == 2, "application mouse compatibility mode did not reload");
            applicationMode.SelectedIndex = 0;
            savePeer.Invoke();
            await Wait(() => !ReadSavedProfile().Settings.TouchModes.ContainsKey("org.waybridge.PrivateEditor"),
                "following connection did not remove the application override");
        }
        finally { activeField.SetValue(settingsController, savedActive); }
        Log("PASS actual WinUI application selector, saved mouse compatibility override, reload and restore connection default preserve independent policies");
        // Reload through the production method used when a connection is selected.
        togglePeer.Toggle();
        Check(!touch.IsOn, "unsaved native touch edit failed");
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [enabled]);
        Check(touch.IsOn, "saved enabled touch did not reload into the actual control");
        togglePeer.Toggle();
        savePeer.Invoke();
        await Wait(() => !ReadSavedProfile().Settings.Touch, "disabled touch was not saved by the GUI");
        var disabled = ReadSavedProfile();
        CheckIndependentPolicies(disabled);
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [disabled]);
        Check(!touch.IsOn, "saved disabled touch did not reload into the actual control");
        Receive(window, TouchStatus(false, true, true));
        Check(effective.Text.Contains("当前连接原生触控已关闭", StringComparison.Ordinal) &&
            !effective.Text.Contains("重连", StringComparison.Ordinal), "negotiated disabled preference still showed a reconnect warning");
        typeof(MainWindow).GetMethod("TouchConnectionChanged", Private)!.Invoke(window, [false]);
        Check(effective.Text.Contains("当前未连接", StringComparison.Ordinal), "disconnect kept a stale touch state");
        Receive(window, TouchStatus(true, true, true));
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [disabled with { Id = Guid.NewGuid() }]);
        Check(effective.Text.Contains("当前未连接", StringComparison.Ordinal), "profile switch kept another connection's touch state");
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [disabled]);
        Check(!Get<SessionController>(window, "controller").IsRunning, "settings persistence unexpectedly connected a session");
        Log("PASS actual WinUI native-touch setting is rendered, accessible, toggles on/off, saves and reloads independently from IME/read-only/files without connecting");
        Log("PASS persistent touch status survives generic success/saved notices, distinguishes saved/effective and readonly states, and resets on disconnect/profile switch");
        await RunProfessionalSettingsAsync(window);
    }
    private static async Task RunProfessionalSettingsAsync(MainWindow window)
    {
        var pen = Get<ToggleSwitch>(window, "pen"); var gamepads = Get<ToggleSwitch>(window, "gamepads");
        var rumble = Get<ToggleSwitch>(window, "gamepadRumble");
        var pressure = Get<NumberBox>(window, "penPressureGamma"); var deadzone = Get<NumberBox>(window, "gamepadDeadzone");
        var penState = Get<TextBlock>(window, "penStatus"); var gamepadState = Get<TextBlock>(window, "gamepadStatus");
        foreach (var (control, id) in new[] { (pen, "NativePenToggle"), (gamepads, "GamepadToggle"), (rumble, "GamepadRumbleToggle") })
        {
            control.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false });
            await Wait(() => control.IsLoaded && control.IsEnabled && control.ActualWidth > 0, id + " not rendered");
            Check(AutomationProperties.GetAutomationId(control) == id && !string.IsNullOrEmpty(AutomationProperties.GetName(control)), id + " inaccessible");
        }
        Check(!pen.IsOn && !gamepads.IsOn && rumble.IsOn && pressure.Value == 1 && deadzone.Value == 15,
            "older GUI profile changed professional input defaults");
        var penPeer = (IToggleProvider)new ToggleSwitchAutomationPeer(pen).GetPattern(PatternInterface.Toggle)!;
        var gamepadPeer = (IToggleProvider)new ToggleSwitchAutomationPeer(gamepads).GetPattern(PatternInterface.Toggle)!;
        var rumblePeer = (IToggleProvider)new ToggleSwitchAutomationPeer(rumble).GetPattern(PatternInterface.Toggle)!;
        var save = Descendants((DependencyObject)window.Content).OfType<Button>().Single(x => AutomationProperties.GetAutomationId(x) == "InputSettingsSave");
        var savePeer = (IInvokeProvider)new ButtonAutomationPeer(save).GetPattern(PatternInterface.Invoke)!;
        penPeer.Toggle(); gamepadPeer.Toggle(); rumblePeer.Toggle(); pressure.Value = 1.75; deadzone.Value = 27;
        savePeer.Invoke();
        await Wait(() => ReadSavedProfile().Settings is { Pen: true, Gamepads: true, GamepadRumble: false, PenPressureGamma: 1.75, GamepadDeadzone: 27 },
            "professional GUI settings did not save on the stable profile");
        var enabled = ReadSavedProfile(); CheckIndependentPolicies(enabled);
        Check(!enabled.Settings.Touch, "professional input settings enabled unrelated touch");
        penPeer.Toggle(); gamepadPeer.Toggle(); pressure.Value = 1; deadzone.Value = 15;
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [enabled]);
        Check(pen.IsOn && gamepads.IsOn && !rumble.IsOn && pressure.Value == 1.75 && deadzone.Value == 27,
            "professional saved controls did not reload");
        Receive(window, new { type = "local_settings_applied", settings = new { read_only = true },
            pen_supported = false, pen_enabled = false, pen_live_supported = false,
            gamepad_supported = false, gamepad_enabled = false, gamepad_live_supported = false, pen_error = "", gamepad_error = "" });
        Check(penState.Text.Contains("当前连接手写笔已关闭", StringComparison.Ordinal) && penState.Text.Contains("暂不支持", StringComparison.Ordinal) &&
            gamepadState.Text.Contains("当前连接手柄转发已关闭", StringComparison.Ordinal) && gamepadState.Text.Contains("暂不支持", StringComparison.Ordinal),
            "professional unsupported state confused saved opt-in with active input");
        Receive(window, new { type = "stream_config", ok = true, fps = 30, bitrate = 12000, codec = "png", read_only = true });
        savePeer.Invoke(); await Wait(() => Get<InfoBar>(window, "notice").Message == "连接设置已保存。", "professional save notice");
        Check(penState.Text.Contains("暂不支持", StringComparison.Ordinal) && gamepadState.Text.Contains("暂不支持", StringComparison.Ordinal),
            "generic success/save erased professional input status");
        Receive(window, new { type = "local_settings_applied", settings = new { read_only = true },
            pen_supported = true, pen_enabled = true, pen_native_active = false, pen_hardware_available = false, pen_live_supported = true,
            gamepad_supported = true, gamepad_enabled = true, gamepad_live_supported = true, gamepad_devices = Array.Empty<object>(),
            rumble_supported = false, pen_error = "", gamepad_error = "" });
        Check(penState.Text.Contains("未检测到可用的手写笔", StringComparison.Ordinal) &&
            gamepadState.Text.Contains("未检测到已连接的手柄", StringComparison.Ordinal) &&
            penState.Text.Contains("只读模式已暂停", StringComparison.Ordinal) && gamepadState.Text.Contains("只读模式已暂停", StringComparison.Ordinal),
            "missing hardware/readonly professional state was hidden");
        Receive(window, new { type = "local_settings_applied", settings = new { read_only = false },
            pen_supported = true, pen_enabled = true, pen_native_active = true, pen_hardware_available = true,
            gamepad_supported = true, gamepad_enabled = true,
            gamepad_devices = new[] { new { userindex = 0, connected = true, name = "Private test controller" } },
            rumble_supported = true, pen_error = "", gamepad_error = "" });
        Check(penState.Text.Contains("当前应用已接入压感手写笔", StringComparison.Ordinal) &&
            gamepadState.Text.Contains("Private test controller", StringComparison.Ordinal) && !penState.Text.Contains("只读模式已暂停", StringComparison.Ordinal),
            "available professional device state failed to recover");
        Receive(window, new { type = "local_settings_applied", gamepad_enabled = true, gamepad_active = false });
        Check(gamepadState.Text.Contains("当前连接手柄转发已开启", StringComparison.Ordinal) &&
            gamepadState.Text.Contains("请先聚焦远端窗口", StringComparison.Ordinal), "background focus lease was confused with the saved gamepad preference");
        Receive(window, new { type = "local_settings_applied", pen_error = "测试笔设备不可用", gamepad_error = "测试手柄不可用" });
        Check(penState.Text.Contains("测试笔设备不可用", StringComparison.Ordinal) && gamepadState.Text.Contains("测试手柄不可用", StringComparison.Ordinal),
            "professional device errors were not retained inline");
        penPeer.Toggle(); savePeer.Invoke(); await Wait(() => !ReadSavedProfile().Settings.Pen, "GUI pen-off save");
        Check(ReadSavedProfile().Settings.Gamepads, "GUI pen-off disabled gamepad");
        gamepadPeer.Toggle(); rumblePeer.Toggle(); savePeer.Invoke();
        await Wait(() => !ReadSavedProfile().Settings.Gamepads && ReadSavedProfile().Settings.GamepadRumble, "GUI gamepad-off save");
        var disabled = ReadSavedProfile(); CheckIndependentPolicies(disabled);
        Check(disabled.Settings.PenPressureGamma == 1.75 && disabled.Settings.GamepadDeadzone == 27, "GUI opt-out discarded calibration");
        typeof(MainWindow).GetMethod("TouchConnectionChanged", Private)!.Invoke(window, [false]);
        Check(penState.Text.Contains("当前未连接", StringComparison.Ordinal) && gamepadState.Text.Contains("当前未连接", StringComparison.Ordinal) &&
            !penState.Text.Contains("测试笔设备不可用", StringComparison.Ordinal), "disconnect retained another connection's device error");
        Receive(window, new { type = "local_settings_applied", pen_enabled = true, gamepad_enabled = true });
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [disabled with { Id = Guid.NewGuid() }]);
        Check(penState.Text.Contains("当前未连接", StringComparison.Ordinal) && gamepadState.Text.Contains("当前未连接", StringComparison.Ordinal), "profile switch leaked professional device state");
        typeof(MainWindow).GetMethod("Load", Private)!.Invoke(window, [disabled]);
        Check(!Get<SessionController>(window, "controller").IsRunning, "professional settings probe unexpectedly connected");
        Log("PASS actual WinUI pen/gamepad controls save/reload independent opt-in, pressure/deadzone/rumble; persistent effective/unsupported/hardware/error/readonly states survive generic notices and reset across profiles");
    }
    private static async Task RunAsync(ControlApplication app)
    {
        MainWindow? window = null; SessionController? controller = null;
        try
        {
            await Wait(() => Field(app, "window") is MainWindow, "MainWindow not launched");
            window = Get<MainWindow>(app, "window");
            controller = Get<SessionController>(window, "controller");
            controller.Status += value => Log("STATUS " + value);
            var picker = Get<ComboBox>(window, "profilePicker");
            await Wait(() => picker.SelectedIndex == 0 && Get<ComboBox>(window, "output").Items.Count > 0, "profile initialize");
            Check(Get<ConnectionProfile>(window, "selected").Id.ToString() == File.ReadAllText(Path.Combine(root, "profile-id.txt")).Trim(), "fixture profile mismatch");
            if (File.Exists(Path.Combine(root, "settings-only")))
            {
                await RunSettingsAsync(window);
                Environment.ExitCode = 0;
                return;
            }
            await Click(window, Get<Button>(window, "connect"));
            await Wait(() => controller.Connected || Get<InfoBar>(window, "notice") is { IsOpen: true, Severity: InfoBarSeverity.Error }, "GUI connection failed", 60000);
            Check(controller.Connected, "GUI connection error: " + Get<InfoBar>(window, "notice").Message);
            var list = Get<ListView>(window, "applicationList");
            await Wait(() => list.Items.Cast<ListViewItem>().Any(x => x.Tag is RemoteApplication a && a.Id == "org.waybridge.FocusProbe.desktop"), "fixture application not catalogued");
            var engine = Field(controller, "engine")!;
            uint worker = (uint)Get<Process>(engine, "process").Id;
            Check(worker != Environment.ProcessId, "native worker must be a different process");
            var nav = Get<NavigationView>(window, "navigation"); nav.SelectedItem = nav.MenuItems[1];
            list.SelectedItem = list.Items.Cast<ListViewItem>().Single(x => x.Tag is RemoteApplication a && a.Id == "org.waybridge.FocusProbe.desktop");
            await Task.Delay(300);
            var launch = Descendants((DependencyObject)window.Content).OfType<Button>().Single(x => x.Content is string text && text == "启动选中应用");
            await Click(window, launch);
            await Wait(() => Remote(GetForegroundWindow(), worker, "Waybridge GUI focus "), "first GUI-launched window did not foreground");
            IntPtr first = GetForegroundWindow(); string firstTitle = Title(first);
            Log($"PASS actual GUI Click -> SSH GIO launch -> worker foreground: {firstTitle}, worker={worker}");
            await Click(window, launch);
            await Wait(() => GetForegroundWindow() != first && Remote(GetForegroundWindow(), worker, "Waybridge GUI focus "), "second process did not foreground");
            IntPtr second = GetForegroundWindow();
            Check(Title(second) != firstTitle, "second launch must have a different Linux process");
            Log($"PASS independent second launch selects new process: {Title(second)}");
            Check(Remote(GetForegroundWindow(), worker, "Waybridge GUI focus "), "refuse keyboard outside owned Qt fixture");
            Send([Key(0x11), Key(0x44), Key(0x44, true), Key(0x11, true)]);
            await Wait(() => Remote(GetForegroundWindow(), worker, "Waybridge GUI modal ") && GetWindow(GetForegroundWindow(), 4) == second, "owned modal did not foreground");
            var modal = GetForegroundWindow(); Log("PASS Qt standard owned modal foreground");
            Check(GetForegroundWindow() == modal && Remote(modal, worker, "Waybridge GUI modal "), "refuse Escape outside owned modal");
            Send([Key(0x1b), Key(0x1b, true)]);
            await Wait(() => GetForegroundWindow() == second && (!IsWindow(modal) || !IsWindowVisible(modal)), "modal close restored another app instead of its owner");
            Check(IsWindow(first), "first app must remain alive during modal restoration");
            Log("PASS modal close restores exact owner with another app alive");
            Environment.ExitCode = 0;
        }
        catch (Exception error)
        {
            Environment.ExitCode = 1; Log(error.ToString());
            if (controller is not null) Log(string.Join('\n', controller.Logs));
        }
        finally
        {
            if (controller is not null) try { await controller.EndSessionAsync(); Log("CLEAN own remote session ended"); } catch (Exception e) { Log("CLEAN end-session: " + e.Message); }
            try { AppNotificationManager.Default.UnregisterAll(); } catch (Exception e) { Log("CLEAN notification registration: " + e.Message); }
            if (!File.Exists(Path.Combine(root, "settings-only")))
            {
                SetCursorPos(originalCursor.x, originalCursor.y);
                if (originalForeground != IntPtr.Zero && IsWindow(originalForeground)) SetForegroundWindow(originalForeground);
            }
            if (window is not null) typeof(MainWindow).GetMethod("TrayAction", Private)!.Invoke(window, ["quit"]);
            else app.Exit();
        }
    }
    [StructLayout(LayoutKind.Sequential)] private struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] private struct MOUSEINPUT { public int x, y; public uint data, flags, time; public UIntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] private struct KEYBDINPUT { public ushort key, scan; public uint flags, time; public UIntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] private struct UNION { [FieldOffset(0)] public MOUSEINPUT mouse; [FieldOffset(0)] public KEYBDINPUT keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct INPUT { public uint type; public UNION input; }
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] private static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr h, ref POINT point);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr h, uint flags);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr h, System.Text.StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr h, System.Text.StringBuilder text, int count);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int height, uint flags);
}
