using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.XamlTypeInfo;
using System.Runtime.CompilerServices;

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
            StartupDiagnostics.Write("acquiring single-instance mutex");
            instance = new Mutex(true, @"Local\Waybridge.ControlCenter", out bool first);
            if (!first) { StartupDiagnostics.Write("another control center is running; forwarding activation"); TrayIcon.ShowExisting(); return; }
            StartGui();
            StartupDiagnostics.Write("application loop exited normally");
        }
        catch (Exception error) { StartupDiagnostics.Fatal("Program.Main", error); Environment.ExitCode = 1; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void StartGui()
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
            new ControlApplication();
        });
    }
}

public sealed class ControlApplication : Application, IXamlMetadataProvider
{
    private MainWindow? window;
    private XamlControlsXamlMetaDataProvider? metadata;
    private readonly HashSet<string> observedTypes = [];
    private XamlControlsXamlMetaDataProvider Metadata => metadata ??= new();
    public ControlApplication()
    {
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
        window = new MainWindow();
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
