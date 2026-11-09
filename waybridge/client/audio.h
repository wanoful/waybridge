#pragma once
// UI/control and video never wait for the renderer: PCM is consumed on its own COM thread.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

class AudioPlayback {
  std::thread worker;
  std::mutex mutex;
  std::deque<int16_t> samples;
  HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::atomic<bool> playing{false};
  static HRESULT endpoint(IMMDeviceEnumerator *devices, IMMDevice **device) {
    DWORD count = GetEnvironmentVariableW(L"WAYBRIDGE_AUDIO_DEVICE", nullptr, 0);
    if (!count) return devices->GetDefaultAudioEndpoint(eRender, eConsole, device);
    if (count > 4096) return E_INVALIDARG;
    std::wstring id(count, L'\0');
    if (!GetEnvironmentVariableW(L"WAYBRIDGE_AUDIO_DEVICE", id.data(), count)) return E_INVALIDARG;
    return devices->GetDevice(id.c_str(), device);
  }
  void render() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
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
      if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
      if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&output));
      UINT capacity = 0;
      if (SUCCEEDED(hr)) hr = client->GetBufferSize(&capacity);
      if (SUCCEEDED(hr)) hr = client->Start();
      if (SUCCEEDED(hr)) {
        playing = true;
        std::cout << "WASAPI 48 kHz stereo audio playback active\n" << std::flush;
        HANDLE events[] = {quit, ready};
        while (WaitForMultipleObjects(2, events, FALSE, 1000) != WAIT_OBJECT_0) {
          UINT padding = 0;
          if (FAILED(client->GetCurrentPadding(&padding)) || padding > capacity) break;
          UINT frames = capacity - padding;
          if (!frames) continue;
          BYTE *bytes = nullptr;
          if (FAILED(output->GetBuffer(frames, &bytes))) break;
          auto dest = reinterpret_cast<int16_t *>(bytes);
          size_t count = size_t(frames) * 2, copied = 0;
          {
            std::lock_guard<std::mutex> guard(mutex);
            while (copied < count && !samples.empty()) {
              dest[copied++] = samples.front(); samples.pop_front();
            }
          }
          memset(dest + copied, 0, (count - copied) * sizeof(int16_t));
          if (FAILED(output->ReleaseBuffer(frames, copied ? 0 : AUDCLNT_BUFFERFLAGS_SILENT))) break;
        }
        client->Stop();
      } else std::cerr << "WASAPI audio unavailable, video continues: " << unsigned(hr) << '\n';
      playing = false;
    }
    CoUninitialize();
  }
public:
  static bool supported() {
    if (GetEnvironmentVariableW(L"WAYBRIDGE_NO_AUDIO", nullptr, 0)) return false;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> devices;
    Microsoft::WRL::ComPtr<IMMDevice> device;
    return SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&devices))) &&
           SUCCEEDED(endpoint(devices.Get(), &device));
  }
  void start() { if (!worker.joinable() && quit && ready) worker = std::thread([this] { render(); }); }
  void push(const uint8_t *bytes, size_t count) {
    if (count % 4 || count > 19200) return;
    std::vector<int16_t> block(count / 2);
    memcpy(block.data(), bytes, count);
    std::lock_guard<std::mutex> guard(mutex);
    while (samples.size() + block.size() > 9600 && !samples.empty()) samples.pop_front();
    samples.insert(samples.end(), block.begin(), block.end());
  }
  void stop() {
    if (quit) SetEvent(quit);
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> guard(mutex); samples.clear();
  }
  ~AudioPlayback() { stop(); if (quit) CloseHandle(quit); if (ready) CloseHandle(ready); }
};
