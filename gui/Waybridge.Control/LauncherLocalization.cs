using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Data;
using Waybridge.Control.Core;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private static readonly ConditionalWeakTable<LocalizedLabel, object> localizedLabels = new();
    private static readonly object activeLabel = new();

    private readonly record struct UiText(string Source, bool Translate, object?[]? Arguments = null)
    {
        public static implicit operator UiText(string text) => new(text, false);
        public string Resolve() => !Translate ? Source : Arguments is null ? Tr(Source) : Tr(Source, Arguments);

        public LocalizedLabel CreateLabel()
        {
            var label = new LocalizedLabel(Resolve);
            localizedLabels.Add(label, activeLabel);
            return label;
        }

        public T Bind<T>(T control, DependencyProperty property) where T : FrameworkElement
        {
            if (Translate) CreateLabel().Bind(control, property);
            else control.SetValue(property, Source);
            return control;
        }

        public void Name(FrameworkElement control) => Bind(control, AutomationProperties.NameProperty);
    }

    private static UiText L(string message) => new(message, true);
    private static UiText L(string message, params object?[] arguments) => new(message, true, arguments);

    private async Task ApplyLanguageAsync(string preference)
    {
        string previous = Localization.Language;
        launcherPreferences.Language = preference;
        Localization.Initialize(preference);
        if (Localization.Language != previous)
        {
            // Update strings in place so editors, focus and the session stay live.
            bool wasLoading = loading;
            loading = true;
            try
            {
                foreach (var entry in localizedLabels) entry.Key.Refresh();
                RefreshLocalizedStatus();
            }
            finally { loading = wasLoading; }
        }
        await SaveLauncherPreferencesAsync();
        await controller.UpdateLanguageAsync();
    }

    private void RefreshLocalizedStatus()
    {
        int outputIndex = output.SelectedIndex;
        audioDevices[0] = ("", Tr("Use the system audio output"));
        output.ItemsSource = audioDevices.Select(device => device.Name).ToList();
        output.SelectedIndex = outputIndex;
        status.Text = controller.CurrentStatus;
        UpdateMode();
        UpdateTouchStatus();
        UpdateInputDeviceStatus();
        UpdateRemoteTrayStatus();
        UpdateApplicationCount();
        UpdateCatalogStatus();
        SetDirectoryLayout();
        UpdateMatchingWindows();
        tray.Update(controller.Connected);
        if (!controller.Connected)
        {
            actual.Text = diagnosticMediaPath.Text = Tr("Waiting for connection");
            foreach (var metric in metrics.Values) metric.Text = Tr("Waiting for capture");
        }
    }
}

[WinRT.GeneratedBindableCustomProperty]
internal sealed partial class LocalizedLabel(Func<string> resolve) : INotifyPropertyChanged
{
    private static readonly PropertyChangedEventArgs textChanged = new(nameof(Text));

    public string Text => resolve();
    public event PropertyChangedEventHandler? PropertyChanged;

    internal void Refresh() => PropertyChanged?.Invoke(this, textChanged);

    internal void Bind(FrameworkElement control, DependencyProperty property) =>
        control.SetBinding(property, new Binding { Source = this, Path = new PropertyPath(nameof(Text)), Mode = BindingMode.OneWay });

    public override string ToString() => Text;
}
