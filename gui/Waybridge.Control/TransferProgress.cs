using System.Text.Json;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Waybridge.Control;

internal sealed partial class MainWindow
{
    private sealed record TransferRow(TextBlock Title, TextBlock Status, ProgressBar Bar, Button Cancel);
    private readonly Dictionary<(string Kind, ulong Id), TransferRow> activeTransfers = [];
    private readonly NumberBox fileRate = Number(0, 0, 1048576);

    private void AddTransferItem(UIElement item)
    {
        transfers.Items.Insert(0, item);
        while (transfers.Items.Count > 100) transfers.Items.RemoveAt(transfers.Items.Count - 1);
        MarkTransferActivity();
    }

    private void UpdateTransferProgress(JsonElement message)
    {
        string kind = message.GetProperty("kind").GetString() ?? "";
        if (kind is not ("upload" or "download" or "drag")) return;
        ulong id = message.GetProperty("id").GetUInt64();
        string name = message.GetProperty("name").GetString() ?? "";
        string state = message.GetProperty("state").GetString() ?? "";
        var key = (kind, id);
        if (!activeTransfers.TryGetValue(key, out var row))
        {
            var title = Heading("", 14);
            var status = Muted("");
            var bar = new ProgressBar { Minimum = 0, Maximum = 100 };
            var cancel = Button("取消");
            cancel.Click += async (_, _) => await GuardAsync(async () =>
            {
                cancel.IsEnabled = false;
                try { await controller.SendEngineAsync(new { type = "cancel_transfer", kind, id }); }
                catch { cancel.IsEnabled = activeTransfers.ContainsKey(key); throw; }
            });
            row = new(title, status, bar, cancel);
            activeTransfers.Add(key, row);
            AddTransferItem(Column(title, status, bar, cancel));
        }

        ulong bytes = message.GetProperty("bytes").GetUInt64();
        ulong total = message.GetProperty("total").GetUInt64();
        string direction = kind switch { "upload" => "发送", "download" => "接收", _ => "拖放接收" };
        row.Title.Text = $"{direction} · {name}";
        bool canCancel = state == "active" && (!message.TryGetProperty("can_cancel", out var allowed) || allowed.GetBoolean());
        row.Cancel.Visibility = canCancel ? Visibility.Visible : Visibility.Collapsed;
        row.Bar.IsIndeterminate = total == 0 && state == "active";
        row.Bar.Value = total > 0 ? Math.Min(100, bytes * 100.0 / total) : state == "done" ? 100 : 0;
        row.Status.Text = state switch
        {
            "active" when !canCancel => "正在完成…",
            "active" => total == 0 ? "正在准备文件…" : $"{FileSize(bytes)} / {FileSize(total)}",
            "done" => $"已完成 · {FileSize(total)}",
            "cancelled" => "已取消",
            _ when message.TryGetProperty("message", out var detail) => "传输失败 · " + detail.GetString(),
            _ => "传输失败"
        };
        if (state != "active")
        {
            row.Bar.IsIndeterminate = false;
            activeTransfers.Remove(key);
        }
    }

    private static string FileSize(ulong bytes) => bytes switch
    {
        < 1024 => $"{bytes} B",
        < 1024 * 1024 => $"{bytes / 1024.0:0.#} KiB",
        < 1024 * 1024 * 1024 => $"{bytes / 1048576.0:0.#} MiB",
        _ => $"{bytes / 1073741824.0:0.##} GiB"
    };

    private void InterruptTransfers()
    {
        foreach (var row in activeTransfers.Values)
        {
            row.Cancel.Visibility = Visibility.Collapsed;
            row.Bar.IsIndeterminate = false;
            row.Status.Text = "连接已断开，传输已中断";
        }
        activeTransfers.Clear();
    }
}
