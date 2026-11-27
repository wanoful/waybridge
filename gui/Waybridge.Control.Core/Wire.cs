using System.Text;
using System.Text.Json;

namespace Waybridge.Control.Core;

public static class Wire
{
    public static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = true,
        MaxDepth = 32
    };
    public static byte[] Encode(object value) => Encoding.UTF8.GetBytes(JsonSerializer.Serialize(value, Json) + "\n");
}

// A peer cannot grow an unbounded ReadLine buffer by withholding a newline.
public sealed class JsonLines(Stream stream, int limit = 1024 * 1024)
{
    private readonly byte[] buffer = new byte[limit];
    private int used;
    public byte[] TakeBufferedBytes()
    {
        if (used == 0) return [];
        byte[] remaining = new byte[used];
        Buffer.BlockCopy(buffer, 0, remaining, 0, used);
        used = 0;
        return remaining;
    }
    public async Task<JsonElement?> ReadAsync(CancellationToken cancellationToken)
    {
        while (true)
        {
            int newline = Array.IndexOf(buffer, (byte)'\n', 0, used);
            if (newline >= 0)
            {
                using var document = JsonDocument.Parse(buffer.AsMemory(0, newline), new() { MaxDepth = 32 });
                var result = document.RootElement.Clone();
                Buffer.BlockCopy(buffer, newline + 1, buffer, 0, used - newline - 1);
                used -= newline + 1;
                return result;
            }
            if (used == buffer.Length) throw new IOException(Tr("Control message is too large"));
            int count = await stream.ReadAsync(buffer.AsMemory(used), cancellationToken);
            if (count == 0)
            {
                if (used != 0) throw new IOException(Tr("The control message was incomplete"));
                return null;
            }
            used += count;
        }
    }
}
