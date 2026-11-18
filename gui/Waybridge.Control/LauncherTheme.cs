using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Markup;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private static class LauncherStyles
    {
        // ThemeResource is resolved at each control, including controls created
        // while a page is detached or while the application theme is overridden.
        private static readonly ResourceDictionary Resources = (ResourceDictionary)XamlReader.Load("""
            <ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
                <Style x:Key="Muted" TargetType="TextBlock">
                    <Setter Property="Foreground" Value="{ThemeResource TextFillColorSecondaryBrush}" />
                </Style>
                <Style x:Key="Card" TargetType="Border">
                    <Setter Property="Background" Value="{ThemeResource CardBackgroundFillColorDefaultBrush}" />
                    <Setter Property="BorderBrush" Value="{ThemeResource CardStrokeColorDefaultBrush}" />
                    <Setter Property="BorderThickness" Value="1" />
                </Style>
                <Style x:Key="Directory" TargetType="Border">
                    <Setter Property="Background" Value="{ThemeResource SolidBackgroundFillColorBaseBrush}" />
                    <Setter Property="BorderBrush" Value="{ThemeResource CardStrokeColorDefaultBrush}" />
                    <Setter Property="BorderThickness" Value="0,0,0,1" />
                </Style>
                <Style x:Key="Transfers" TargetType="SplitView">
                    <Setter Property="PaneBackground" Value="{ThemeResource SolidBackgroundFillColorBaseBrush}" />
                </Style>
            </ResourceDictionary>
            """);

        public static Style Muted { get; } = (Style)Resources["Muted"];
        public static Style Card { get; } = (Style)Resources["Card"];
        public static Style Directory { get; } = (Style)Resources["Directory"];
        public static Style Transfers { get; } = (Style)Resources["Transfers"];
    }

    private void ApplyLauncherTheme()
    {
        var (content, titleBar) = launcherPreferences.Theme switch
        {
            "light" => (ElementTheme.Light, TitleBarTheme.Light),
            "dark" => (ElementTheme.Dark, TitleBarTheme.Dark),
            _ => (ElementTheme.Default, TitleBarTheme.UseDefaultAppMode)
        };
        root.RequestedTheme = content;
        if (AppWindowTitleBar.IsCustomizationSupported()) AppWindow.TitleBar.PreferredTheme = titleBar;
    }
}
