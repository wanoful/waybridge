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
        Localization.Initialize(LauncherPreferences.StartupLanguage(LauncherPreferences.DefaultDirectory));
        StartupDiagnostics.Install();
        try
        {
            AppLaunchLink? launchLink = null;
            bool startHidden = false;
            if (args.Length == 1 && args[0] == "--background")
            {
                startHidden = true;
            }
            else if (args.Length > 0 && (args[0] != "--launch-link" || args.Length != 2 ||
                !AppLaunchLink.TryParse(args[1], out launchLink)))
            {
                TrayIcon.ShowLinkError(Tr("Invalid app link. Copy it again or create a new shortcut from Waybridge."));
                Environment.ExitCode = 2;
                return;
            }
            StartupDiagnostics.Write("acquiring single-instance mutex");
            instance = new Mutex(true, @"Local\Waybridge.ControlCenter", out bool first);
            if (!first)
            {
                if (startHidden) return;
                StartupDiagnostics.Write("another control center is running; forwarding activation");
                if (launchLink is null) TrayIcon.ShowExisting();
                else if (!TrayIcon.ForwardLaunchLink(launchLink))
                {
                    TrayIcon.ShowLinkError(Tr("Waybridge cannot receive app links right now. Try again shortly."));
                    Environment.ExitCode = 1;
                }
                return;
            }
            try { AppShortcuts.RegisterProtocol(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or SecurityException)
            {
                StartupDiagnostics.Write("unable to register the Waybridge URI protocol: " + error.Message);
            }
            StartGui(launchLink, startHidden);
            StartupDiagnostics.Write("application loop exited normally");
        }
        catch (Exception error) { StartupDiagnostics.Fatal("Program.Main", error); Environment.ExitCode = 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void StartGui(AppLaunchLink? launchLink, bool startHidden)
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
            new ControlApplication(null, launchLink, startHidden);
        });
    }
}

public sealed class ControlApplication : Application, IXamlMetadataProvider
{
    private MainWindow? window;
    private XamlControlsXamlMetaDataProvider? metadata;
    private readonly string? stateDirectory;
    private readonly AppLaunchLink? launchLink;
    private readonly bool startHidden;
    private readonly HashSet<string> observedTypes = [];
    private XamlControlsXamlMetaDataProvider Metadata => metadata ??= new();
    public ControlApplication(string? stateDirectory = null) : this(stateDirectory, null, false)
    {
        if (stateDirectory is not null) Localization.Initialize(LauncherPreferences.StartupLanguage(stateDirectory));
    }

    internal ControlApplication(string? stateDirectory, AppLaunchLink? launchLink, bool startHidden)
    {
        this.stateDirectory = stateDirectory;
        this.launchLink = launchLink;
        this.startHidden = startHidden;
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
        if (startHidden)
        {
            StartupDiagnostics.Write("starting hidden in tray for background IPC");
            window.StartBackgroundInitialization();
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
