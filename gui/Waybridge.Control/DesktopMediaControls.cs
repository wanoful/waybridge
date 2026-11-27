using System.Runtime.InteropServices;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.UI.Dispatching;
using Waybridge.Control.Core;
using Windows.Graphics.Imaging;
using Windows.Media;
using Windows.Storage.Streams;

namespace Waybridge.Control;

/// <summary>
/// Bridges Linux MPRIS2 playback metadata and transport controls to Windows
/// System Media Transport Controls (SMTC). All state transitions are serialized
/// on one bounded worker queue; SMTC object mutations run on the window's UI
/// dispatcher so HWND-bound interop never violates apartment affinity.
/// </summary>
internal sealed class DesktopMediaControls : IAsyncDisposable
{
    private const uint MaxArtworkDimension = 2048;
    private const ulong MaxArtworkPixels = 4 * 1024 * 1024;
    private const double MaxThumbnailEdge = 256.0;

    private readonly DispatcherQueue dispatcher;
    private readonly Func<object, Task> send;
    private readonly Action<string> status;
    private readonly Channel<object> queue = Channel.CreateBounded<object>(new BoundedChannelOptions(128)
        { SingleReader = true, FullMode = BoundedChannelFullMode.DropOldest });
    private readonly CancellationTokenSource lifetime = new();
    private readonly SystemMediaTransportControls? smtc;
    private readonly Task worker;
    private readonly bool available;

    private bool connected, enabled = true;
    private RemoteMediaState? currentState;
    private (string Player, ulong Generation, ulong Revision)? appliedArt;
    private InMemoryRandomAccessStream? activeThumbnailStream;

    private sealed record Connection(bool Connected);
    private sealed record Sharing(bool Enabled);
    private sealed record ButtonAction(string Action);
    private sealed record SeekAction(long PositionUs);

    public DesktopMediaControls(IntPtr hwnd, DispatcherQueue dispatcher, Func<object, Task> send, Action<string> status)
    {
        this.dispatcher = dispatcher;
        this.send = send;
        this.status = status;
        try
        {
            smtc = SystemMediaTransportControlsInterop.GetForWindow(hwnd);
            smtc.IsEnabled = false;
            smtc.PlaybackStatus = MediaPlaybackStatus.Closed;
            smtc.ButtonPressed += OnButtonPressed;
            smtc.PlaybackPositionChangeRequested += OnPositionChangeRequested;
            available = true;
        }
        catch (Exception error)
        {
            if (smtc is not null)
            {
                try { smtc.ButtonPressed -= OnButtonPressed; } catch (COMException) { }
                try { smtc.PlaybackPositionChangeRequested -= OnPositionChangeRequested; } catch (COMException) { }
            }
            smtc = null;
            available = false;
            StartupDiagnostics.Write($"Windows SMTC unavailable (0x{error.HResult:X8}): {error.Message}");
        }
        worker = Task.Run(LoopAsync);
    }

    private void Enqueue(object item)
    {
        if (lifetime.IsCancellationRequested) return;
        queue.Writer.TryWrite(item);
    }

    public void ConnectionChanged(bool value) => Enqueue(new Connection(value));
    public void SharingChanged(bool value) => Enqueue(new Sharing(value));

    public bool Receive(JsonElement message)
    {
        if (!message.TryGetProperty("type", out var typeProp) || typeProp.ValueKind != JsonValueKind.String)
            return false;
        string? type = typeProp.GetString();
        if (type is not ("media_state" or "media_art" or "media_reset")) return false;
        Enqueue(message.Clone());
        return true;
    }

    private void OnButtonPressed(SystemMediaTransportControls sender, SystemMediaTransportControlsButtonPressedEventArgs args)
    {
        string? action = args.Button switch
        {
            SystemMediaTransportControlsButton.Play => "play",
            SystemMediaTransportControlsButton.Pause => "pause",
            SystemMediaTransportControlsButton.Stop => "stop",
            SystemMediaTransportControlsButton.Next => "next",
            SystemMediaTransportControlsButton.Previous => "previous",
            _ => null
        };
        if (action is not null) Enqueue(new ButtonAction(action));
    }

    private void OnPositionChangeRequested(SystemMediaTransportControls sender, PlaybackPositionChangeRequestedEventArgs args)
    {
        long positionUs = Math.Max(0, args.RequestedPlaybackPosition.Ticks / 10);
        Enqueue(new SeekAction(positionUs));
    }

    private async Task LoopAsync()
    {
        try
        {
            await foreach (var item in queue.Reader.ReadAllAsync(lifetime.Token))
            {
                switch (item)
                {
                    case Connection connection:
                        connected = connection.Connected;
                        await ResetAsync();
                        if (connected) await ReportAvailabilityAsync();
                        break;
                    case Sharing sharing:
                        enabled = sharing.Enabled;
                        if (!enabled) await ResetAsync();
                        break;
                    case JsonElement message:
                        await HandleMessageAsync(message);
                        break;
                    case ButtonAction action:
                        await SendActionAsync(action.Action);
                        break;
                    case SeekAction seek:
                        await SendSeekAsync(seek.PositionUs);
                        break;
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception error)
        {
            StartupDiagnostics.Write($"DesktopMediaControls loop failed: {error.Message}");
            status(Tr("Windows media controls failed. Forwarding is disabled until you reconnect."));
        }
        finally
        {
            try { if (connected) await send(new { type = "media_controls_available", available = false }); } catch { }
            try { await ResetAsync(); } catch { }
        }
    }

    private async Task ReportAvailabilityAsync()
    {
        try { await send(new { type = "media_controls_available", available }); } catch { }
    }

    private async Task HandleMessageAsync(JsonElement message)
    {
        string? type = message.GetProperty("type").GetString();
        if (type == "media_reset")
        {
            await ResetAsync();
            return;
        }
        if (!connected || !enabled || !available || smtc is null) return;
        if (type == "media_state")
        {
            if (!MediaControlsModel.TryParseState(message, out var state) || state is null) return;
            await ApplyStateAsync(state);
        }
        else if (type == "media_art")
        {
            if (!MediaControlsModel.TryParseArt(message, currentState, out var bytes, out _, out ulong revision))
                return;
            var expected = currentState!;
            var stream = await PrepareThumbnailAsync(bytes);
            if (stream is null) return;
            // Re-verify state didn't change while decoding artwork off the UI thread.
            if (!connected || !enabled || currentState is null ||
                currentState.Epoch != expected.Epoch ||
                currentState.Player != expected.Player ||
                currentState.Generation != expected.Generation ||
                currentState.ArtRevision != revision)
            {
                stream.Dispose();
                return;
            }
            bool applied = await ApplyThumbnailAsync(stream);
            if (applied)
                appliedArt = (expected.Player, expected.Generation, revision);
            else
                stream.Dispose();
        }
    }

    private async Task ApplyStateAsync(RemoteMediaState state)
    {
        currentState = state;
        bool keepThumbnail = state.ArtRevision != 0 &&
            appliedArt == (state.Player, state.Generation, state.ArtRevision);
        if (!keepThumbnail) appliedArt = null;

        await RunOnUiAsync(() =>
        {
            if (smtc is null) return;
            smtc.IsEnabled = true;
            smtc.IsPlayEnabled = state.EffectiveCanPlay;
            smtc.IsPauseEnabled = state.EffectiveCanPause;
            smtc.IsStopEnabled = state.EffectiveCanStop;
            smtc.IsNextEnabled = state.EffectiveCanNext;
            smtc.IsPreviousEnabled = state.EffectiveCanPrevious;
            smtc.PlaybackStatus = state.Status switch
            {
                "Playing" => MediaPlaybackStatus.Playing,
                "Paused" => MediaPlaybackStatus.Paused,
                _ => MediaPlaybackStatus.Stopped
            };

            var updater = smtc.DisplayUpdater;
            if (!keepThumbnail)
            {
                updater.Thumbnail = null;
                activeThumbnailStream?.Dispose();
                activeThumbnailStream = null;
            }
            updater.Type = MediaPlaybackType.Music;
            updater.MusicProperties.Title = state.DisplayTitle;
            updater.MusicProperties.Artist = state.DisplayArtist;
            updater.MusicProperties.AlbumTitle = state.DisplayAlbum;
            updater.Update();

            var timeline = new SystemMediaTransportControlsTimelineProperties();
            if (state.LengthUs > 0)
            {
                var duration = TimeSpan.FromTicks(state.LengthUs * 10);
                long posUs = Math.Clamp(state.PositionUs ?? 0, 0, state.LengthUs);
                timeline.StartTime = TimeSpan.Zero;
                timeline.EndTime = duration;
                timeline.Position = TimeSpan.FromTicks(posUs * 10);
                timeline.MinSeekTime = TimeSpan.Zero;
                timeline.MaxSeekTime = state.EffectiveCanSeek ? duration : TimeSpan.Zero;
            }
            smtc.UpdateTimelineProperties(timeline);
        });
    }

    private async Task<bool> ApplyThumbnailAsync(InMemoryRandomAccessStream nextStream)
    {
        bool ok = false;
        await RunOnUiAsync(() =>
        {
            if (smtc is null) return;
            var previous = activeThumbnailStream;
            activeThumbnailStream = nextStream;
            var updater = smtc.DisplayUpdater;
            updater.Thumbnail = RandomAccessStreamReference.CreateFromStream(nextStream);
            updater.Update();
            previous?.Dispose();
            ok = true;
        });
        return ok;
    }

    private static async Task<InMemoryRandomAccessStream?> PrepareThumbnailAsync(byte[] bytes)
    {
        try
        {
            using var input = new InMemoryRandomAccessStream();
            using (var writer = new DataWriter(input.GetOutputStreamAt(0)))
            {
                writer.WriteBytes(bytes);
                await writer.StoreAsync();
            }
            input.Seek(0);
            var decoder = await BitmapDecoder.CreateAsync(input);
            if (decoder.PixelWidth == 0 || decoder.PixelHeight == 0 ||
                decoder.PixelWidth > MaxArtworkDimension || decoder.PixelHeight > MaxArtworkDimension ||
                (ulong)decoder.PixelWidth * decoder.PixelHeight > MaxArtworkPixels)
                return null;

            double scale = Math.Min(1.0, MaxThumbnailEdge / Math.Max(decoder.PixelWidth, decoder.PixelHeight));
            uint width = Math.Clamp((uint)Math.Round(decoder.PixelWidth * scale), 1u, (uint)MaxThumbnailEdge);
            uint height = Math.Clamp((uint)Math.Round(decoder.PixelHeight * scale), 1u, (uint)MaxThumbnailEdge);
            var pixels = (await decoder.GetPixelDataAsync(
                BitmapPixelFormat.Bgra8,
                BitmapAlphaMode.Premultiplied,
                new BitmapTransform
                {
                    ScaledWidth = width,
                    ScaledHeight = height,
                    InterpolationMode = BitmapInterpolationMode.Fant
                },
                ExifOrientationMode.RespectExifOrientation,
                ColorManagementMode.DoNotColorManage)).DetachPixelData();

            var output = new InMemoryRandomAccessStream();
            try
            {
                var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, output);
                encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, width, height, 96, 96, pixels);
                await encoder.FlushAsync();
                output.Seek(0);
                return output;
            }
            catch
            {
                output.Dispose();
                return null;
            }
        }
        catch
        {
            return null;
        }
    }

    private async Task SendActionAsync(string action)
    {
        if (!connected || !enabled || !available) return;
        var command = MediaControlsModel.BuildActionCommand(currentState, action);
        if (command is null) return;
        try { await send(command); } catch { }
    }

    private async Task SendSeekAsync(long positionUs)
    {
        if (!connected || !enabled || !available) return;
        var command = MediaControlsModel.BuildSeekCommand(currentState, positionUs);
        if (command is null) return;
        try { await send(command); } catch { }
    }

    private Task ResetAsync()
    {
        currentState = null;
        appliedArt = null;
        return RunOnUiAsync(ClearSmtcOnUi);
    }

    private void ClearSmtcOnUi()
    {
        if (smtc is null) return;
        try
        {
            smtc.IsPlayEnabled = false;
            smtc.IsPauseEnabled = false;
            smtc.IsStopEnabled = false;
            smtc.IsNextEnabled = false;
            smtc.IsPreviousEnabled = false;
            smtc.PlaybackStatus = MediaPlaybackStatus.Closed;
            smtc.IsEnabled = false;
            smtc.DisplayUpdater.ClearAll();
            smtc.DisplayUpdater.Update();
            smtc.UpdateTimelineProperties(new SystemMediaTransportControlsTimelineProperties());
        }
        catch (COMException) { }
        activeThumbnailStream?.Dispose();
        activeThumbnailStream = null;
    }

    private Task RunOnUiAsync(Action action)
    {
        if (dispatcher.HasThreadAccess)
        {
            try { action(); } catch (COMException) { }
            return Task.CompletedTask;
        }
        var done = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!dispatcher.TryEnqueue(() =>
        {
            try { action(); }
            catch (COMException) { }
            finally { done.TrySetResult(); }
        }))
        {
            done.TrySetResult();
        }
        return done.Task;
    }

    public async ValueTask DisposeAsync()
    {
        queue.Writer.TryComplete();
        lifetime.Cancel();
        try
        {
            try { await worker; } catch (OperationCanceledException) { }
            await RunOnUiAsync(() =>
            {
                if (smtc is not null)
                {
                    try { smtc.ButtonPressed -= OnButtonPressed; } catch (COMException) { }
                    try { smtc.PlaybackPositionChangeRequested -= OnPositionChangeRequested; } catch (COMException) { }
                }
                ClearSmtcOnUi();
            });
        }
        finally
        {
            lifetime.Dispose();
        }
    }
}
