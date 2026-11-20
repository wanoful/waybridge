using System.Text.Json;
using Microsoft.UI.Xaml.Controls;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private readonly ToggleSwitch microphone = Toggle(false);
    private readonly TextBlock microphoneStatus = Muted("当前未连接。");
    private string? liveMicrophoneStatus;

    private void ReceiveMicrophoneStatus(JsonElement message)
    {
        if (TouchFlag(message, "microphone_supported") is not { } supported) return;
        bool enabled = TouchFlag(message, "microphone_enabled") == true;
        string error = message.TryGetProperty("microphone_error", out var reason) ? reason.GetString() ?? "" : "";
        liveMicrophoneStatus = !enabled ? "当前连接已关闭麦克风转发。"
            : TouchFlag(message, "microphone_paused") == true ? "已暂停。当前连接处于只读模式。"
            : !supported ? "当前 Linux 会话没有麦克风输入。"
            : TouchFlag(message, "microphone_active") == true ? "正在转发 Windows 默认麦克风。"
            : error.Length > 0 ? error : "正在连接 Windows 默认麦克风…";
        UpdateMicrophoneStatus();
    }

    private void UpdateMicrophoneStatus()
    {
        if (!controller.Connected || controller.ActiveProfileId != selected.Id)
            liveMicrophoneStatus = null;
        microphoneStatus.Text = liveMicrophoneStatus ?? (controller.Connected
            ? "等待当前连接报告麦克风状态。" : "当前未连接。");
    }
}
