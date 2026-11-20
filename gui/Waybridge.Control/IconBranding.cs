using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Streams;

namespace Waybridge.Control;

internal static class IconBranding
{
    // Import once, preserve aspect ratio and transparency, then store the small
    // normalized PNG in the profile. The original file can be moved or removed.
    public static async Task<string> NormalizeAsync(StorageFile file)
    {
        using var input = await file.OpenReadAsync();
        if (input.Size == 0 || input.Size > 8 * 1024 * 1024) throw new ArgumentException(Tr("The badge image must be 8 MiB or smaller"));
        var decoder = await BitmapDecoder.CreateAsync(input);
        if (decoder.PixelWidth == 0 || decoder.PixelHeight == 0 || decoder.PixelWidth > 8192 || decoder.PixelHeight > 8192 ||
            (ulong)decoder.PixelWidth * decoder.PixelHeight > 16 * 1024 * 1024) throw new ArgumentException(Tr("The badge image dimensions are too large"));
        double ratio = 64.0 / Math.Max(decoder.PixelWidth, decoder.PixelHeight);
        uint width = Math.Clamp((uint)Math.Round(decoder.PixelWidth * ratio), 1u, 64u);
        uint height = Math.Clamp((uint)Math.Round(decoder.PixelHeight * ratio), 1u, 64u);
        var source = (await decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
            new BitmapTransform { ScaledWidth = width, ScaledHeight = height, InterpolationMode = BitmapInterpolationMode.Fant },
            ExifOrientationMode.IgnoreExifOrientation, ColorManagementMode.DoNotColorManage)).DetachPixelData();
        var square = new byte[64 * 64 * 4];
        for (int row = 0; row < height; ++row)
            System.Buffer.BlockCopy(source, row * (int)width * 4, square, ((row + (64 - (int)height) / 2) * 64 + (64 - (int)width) / 2) * 4, (int)width * 4);
        using var output = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, output);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, 64, 64, 96, 96, square);
        await encoder.FlushAsync();
        if (output.Size > 24 * 1024) throw new ArgumentException(Tr("Could not encode the custom badge"));
        output.Seek(0);
        using var reader = new DataReader(output.GetInputStreamAt(0));
        await reader.LoadAsync((uint)output.Size);
        var png = new byte[(int)output.Size]; reader.ReadBytes(png);
        return Convert.ToBase64String(png);
    }
    public static async Task<BitmapImage> PreviewAsync(string png)
    {
        var image = new BitmapImage();
        byte[] bytes = png.Length == 0 ? await File.ReadAllBytesAsync(Path.Combine(AppContext.BaseDirectory, "Assets", "waybridge-badge.png")) : Convert.FromBase64String(png);
        using var stream = new InMemoryRandomAccessStream();
        using (var writer = new DataWriter(stream.GetOutputStreamAt(0))) { writer.WriteBytes(bytes); await writer.StoreAsync(); }
        stream.Seek(0); await image.SetSourceAsync(stream); return image;
    }
}
