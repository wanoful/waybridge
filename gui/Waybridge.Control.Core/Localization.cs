using System.Globalization;
using System.Text.Json;

namespace Waybridge.Control.Core;

public static class Localization
{
    private static readonly Lazy<Dictionary<string, string>> Chinese = new(() =>
    {
        using var stream = typeof(Localization).Assembly.GetManifestResourceStream("Waybridge.zh-Hans.json")!;
        return JsonSerializer.Deserialize<Dictionary<string, string>>(stream)!;
    });

    private static volatile string language = Resolve("system");
    public static string Language => language;

    public static void Initialize(string preference) => language = Resolve(preference);

    private static string Resolve(string preference) => preference switch
    {
        "en" or "zh-Hans" => preference,
        _ => CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "zh" ? "zh-Hans" : "en"
    };

    public static string Tr(string message) => Language == "zh-Hans"
        ? Chinese.Value.GetValueOrDefault(message, message)
        : message;

    public static string Tr(string message, params object?[] arguments) =>
        string.Format(CultureInfo.CurrentCulture, Tr(message), arguments);

    public static string TrDiagnostic(string message)
    {
        if (Language != "zh-Hans") return message;
        if (Chinese.Value.TryGetValue(message, out var translated)) return translated;
        // Native diagnostics may append an OS or library error to a known description.
        int split = message.IndexOf(": ", StringComparison.Ordinal);
        if (split >= 0 && Chinese.Value.TryGetValue(message[..(split + 2)], out translated))
            return translated + message[(split + 2)..];
        return message;
    }
}
