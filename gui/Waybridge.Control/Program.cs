using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.XamlTypeInfo;
using System.Runtime.CompilerServices;
using System.Security;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal static class Program
{
    private static Mutex? instance;
    [STAThread]
    public static void Main(string[] args)
    {
        StartupDiagnostics.Install();
        try
        {
            AppLaunchLink? launchLink = null;
            if (args.Length > 0 && (args[0] != "--launch-link" || args.Length != 2 ||
                !AppLaunchLink.TryParse(args[1], out launchLink)))
            {
                TrayIcon.ShowLinkError("应用链接无效。请在 Waybridge 中重新复制应用链接或创建快捷方式。");
                Environment.ExitCode = 2;
                return;
            }
            StartupDiagnostics.Write("acquiring single-instance mutex");
            instance = new Mutex(true, @"Local\Waybridge.ControlCenter", out bool first);
            if (!first)
            {
                StartupDiagnostics.Write("another control center is running; forwarding activation");
                if (launchLink is null) TrayIcon.ShowExisting();
                else if (!TrayIcon.ForwardLaunchLink(launchLink))
                {
                    TrayIcon.ShowLinkError("Waybridge 暂时无法接收应用链接，请稍后重试。");
                    Environment.ExitCode = 1;
                }
                return;
            }
            try { AppShortcuts.RegisterProtocol(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or SecurityException)
            {
                StartupDiagnostics.Write("unable to register the Waybridge URI protocol: " + error.Message);
            }
            StartGui(launchLink);
            StartupDiagnostics.Write("application loop exited normally");
        }
        catch (Exception error) { StartupDiagnostics.Fatal("Program.Main", error); Environment.ExitCode = 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void StartGui(AppLaunchLink? launchLink)
    {
        StartupDiagnostics.Write("initializing WinRT COM wrappers");
        WinRT.ComWrappersSupport.InitializeComWrappers();
        StartupDiagnostics.Write("entering WinUI Application.Start");
        Application.Start(_ =>
        {
            StartupDiagnostics.Write("WinUI dispatcher initialized");
            SynchronizationContext.SetSynchronizationContext(
                new Microsoft.UI.Dispatching.DispatcherQueueSynchronizationContext(
                    Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread()));
            new ControlApplication(null, launchLink);
        });
    }
}

public sealed class ControlApplication : Application, IXamlMetadataProvider
{
    private MainWindow? window;
    private XamlControlsXamlMetaDataProvider? metadata;
    private readonly string? stateDirectory;
    private readonly AppLaunchLink? launchLink;
    private readonly HashSet<string> observedTypes = [];
    private XamlControlsXamlMetaDataProvider Metadata => metadata ??= new();
    public ControlApplication(string? stateDirectory = null) : this(stateDirectory, null) { }

    internal ControlApplication(string? stateDirectory, AppLaunchLink? launchLink)
    {
        this.stateDirectory = stateDirectory;
        this.launchLink = launchLink;
        StartupDiagnostics.Write("WinUI Application constructed");
        UnhandledException += (_, args) => StartupDiagnostics.Fatal("WinUI unhandled exception", args.Exception);
    }
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // These steps are normally supplied by the generated App XAML bootstrap.
        // A programmatic application must initialize and expose the control
        // library's type provider before its compiled themes are instantiated.
        StartupDiagnostics.Write("initializing WinUI control metadata (bootstrap v2)");
        XamlControlsXamlMetaDataProvider.Initialize();
        StartupDiagnostics.Write("WinUI control metadata initialized");
        StartupDiagnostics.Write("loading XamlControlsResources");
        Resources.MergedDictionaries.Add(new Microsoft.UI.Xaml.Controls.XamlControlsResources());
        StartupDiagnostics.Write("XamlControlsResources loaded and merged");
        StartupDiagnostics.Write("creating MainWindow");
        window = new MainWindow(stateDirectory);
        if (launchLink is not null)
        {
            window.QueueLaunchLink(launchLink);
            return;
        }
        StartupDiagnostics.Write("activating MainWindow");
        window.Activate();
        StartupDiagnostics.Write("MainWindow activated");
    }
    public IXamlType GetXamlType(Type type)
    {
        var result = Metadata.GetXamlType(type);
        TraceType(type.FullName ?? type.Name, result is not null);
        return result!;
    }
    public IXamlType GetXamlType(string fullName)
    {
        var result = Metadata.GetXamlType(fullName);
        TraceType(fullName, result is not null);
        return result!;
    }
    public XmlnsDefinition[] GetXmlnsDefinitions() => Metadata.GetXmlnsDefinitions();
    private void TraceType(string name, bool resolved)
    {
        if (observedTypes.Count < 64 && observedTypes.Add(name))
            StartupDiagnostics.Write($"WinUI metadata: {name} = {(resolved ? "resolved" : "not found")}");
    }
}
