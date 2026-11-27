#pragma once
// UI/control and video never wait for the renderer: PCM is consumed on its own COM thread.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <wrl/client.h>
#include "audio_playout.h"
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

class AudioPlayback {
  std::thread worker;
  std::mutex mutex;
  AudioPlayout playout;
  const char *codec = nullptr;
  uint64_t skippedPackets = 0;
  HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::atomic<bool> playing{false};
  std::atomic<float> volume{1.0f};
  static HRESULT endpoint(IMMDeviceEnumerator *devices, IMMDevice **device) {
    DWORD count = GetEnvironmentVariableW(L"WAYBRIDGE_AUDIO_DEVICE", nullptr, 0);
    if (!count) return devices->GetDefaultAudioEndpoint(eRender, eConsole, device);
    if (count > 4096) return E_INVALIDARG;
    std::wstring id(count, L'\0');
    if (!GetEnvironmentVariableW(L"WAYBRIDGE_AUDIO_DEVICE", id.data(), count)) return E_INVALIDARG;
    return devices->GetDevice(id.c_str(), device);
  }
  HRESULT renderEndpoint() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> output;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devices));
    if (SUCCEEDED(hr)) hr = endpoint(devices.Get(), &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                             nullptr, reinterpret_cast<void **>(client.GetAddressOf()));
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2;
    format.nSamplesPerSec = 48000; format.wBitsPerSample = 16;
    format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
        200000, 0, &format, nullptr);
    ResetEvent(ready);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&output));
    UINT capacity = 0;
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&capacity);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) return hr;

    playing = true;
    std::cout << "WASAPI 48 kHz stereo audio playback active\n" << std::flush;
    HANDLE events[] = {quit, ready};
    while (true) {
      DWORD signalled = WaitForMultipleObjects(2, events, FALSE, 1000);
      if (signalled == WAIT_OBJECT_0) break;
      if (signalled == WAIT_FAILED) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        break;
      }
      UINT padding = 0;
      hr = client->GetCurrentPadding(&padding);
      if (FAILED(hr) || padding > capacity) break;
      UINT frames = capacity - padding;
      if (!frames) continue;
      BYTE *bytes = nullptr;
      hr = output->GetBuffer(frames, &bytes);
      if (FAILED(hr)) break;
      auto dest = reinterpret_cast<int16_t *>(bytes);
      size_t count = size_t(frames) * 2, copied;
      {
        std::lock_guard<std::mutex> guard(mutex);
        copied = playout.read(dest, count, volume.load(std::memory_order_relaxed));
      }
      if (copied) memset(dest + copied, 0, (count - copied) * sizeof(int16_t));
      hr = output->ReleaseBuffer(frames, copied ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
      if (FAILED(hr)) break;
    }
    client->Stop();
    playing = false;
    return hr;
  }
  void render() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init)) return;
    HRESULT previousError = S_OK;
    while (WaitForSingleObject(quit, 0) == WAIT_TIMEOUT) {
      const HRESULT hr = renderEndpoint();
      if (FAILED(hr) && hr != previousError)
        std::cerr << "WASAPI audio unavailable, retrying: " << unsigned(hr) << '\n';
      previousError = hr;
      {
        std::lock_guard<std::mutex> guard(mutex);
        playout.rebuffer();
      }
      if (WaitForSingleObject(quit, 1000) != WAIT_TIMEOUT) break;
    }
    CoUninitialize();
  }
public:
  struct Status {
    const char *codec;
    bool playing;
    double bufferedMs;
    uint64_t skippedPackets, underruns;
    double discardedMs;
  };
  Status status() {
    std::lock_guard<std::mutex> guard(mutex);
    return {codec, playing.load(), double(playout.bufferedSamples()) / AudioPlayout::SamplesPerMs,
            skippedPackets, playout.underrunCount(),
            double(playout.discardedSampleCount()) / AudioPlayout::SamplesPerMs};
  }
  void setVolume(float value) { volume.store(std::max(0.0f, std::min(1.0f, value))); }
  static bool supported() {
    if (GetEnvironmentVariableW(L"WAYBRIDGE_NO_AUDIO", nullptr, 0)) return false;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> devices;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&devices))) &&
           SUCCEEDED(endpoint(devices.Get(), &device));
  }
  void start(const char *codecName) {
    if (worker.joinable() || !quit || !ready) return;
    ResetEvent(quit);
    std::lock_guard<std::mutex> guard(mutex);
    codec = codecName;
    worker = std::thread([this] { render(); });
  }
  void push(const uint8_t *bytes, size_t count, uint64_t skipped) {
    if (!count || count % 4 || count > AudioPlayout::MaxSamples * sizeof(int16_t)) return;
    std::vector<int16_t> block(count / 2);
    memcpy(block.data(), bytes, count);
    std::lock_guard<std::mutex> guard(mutex);
    skippedPackets += skipped;
    playout.push(block.data(), block.size());
  }
  void stop() {
    if (quit) SetEvent(quit);
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> guard(mutex);
    playout.clear();
  }
  ~AudioPlayback() { stop(); if (quit) CloseHandle(quit); if (ready) CloseHandle(ready); }
};
