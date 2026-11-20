using System.Text.Json;
using Microsoft.UI.Xaml.Controls;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private readonly ToggleSwitch microphone = Toggle(false);
    private readonly TextBlock microphoneStatus = Muted(Tr("No active connection."));
    private JsonElement? microphoneReport;

    private void ReceiveMicrophoneStatus(JsonElement message)
    {
        if (TouchFlag(message, "microphone_supported") is null) return;
        microphoneReport = message;
        UpdateMicrophoneStatus();
    }

    private void UpdateMicrophoneStatus()
    {
        bool connected = controller.Connected && controller.ActiveProfileId == selected.Id;
        if (!connected) microphoneReport = null;
        if (microphoneReport is not { } message)
        {
            microphoneStatus.Text = connected ? Tr("Waiting for this connection to report microphone status.") : Tr("No active connection.");
            return;
        }
        bool enabled = TouchFlag(message, "microphone_enabled") == true;
        string error = message.TryGetProperty("microphone_error", out var reason) ? TrDiagnostic(reason.GetString() ?? "") : "";
        microphoneStatus.Text = !enabled ? Tr("Microphone forwarding is off for this connection.")
            : TouchFlag(message, "microphone_paused") == true ? Tr("Paused because this connection is view-only.")
            : TouchFlag(message, "microphone_supported") == false ? Tr("The Linux session has no microphone input.")
            : TouchFlag(message, "microphone_active") == true ? Tr("Forwarding the default Windows microphone.")
            : error.Length > 0 ? error : Tr("Connecting to the default Windows microphone…");
    }
}
