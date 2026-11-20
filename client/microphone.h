#pragma once
// One WASAPI capture endpoint and one authenticated socket, off the UI thread.
#include "native_transport.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class MicrophoneForwarder {
public:
  struct Lease {
    sockaddr_storage address{};
    int addressSize = sizeof(address);
    std::string host, token, key;
    uint64_t request = 0;
  };
  struct Status { bool active = false; std::string error; };

private:
  static constexpr size_t frames = 480;
  static constexpr uint8_t kind = 3;
  static constexpr size_t credits = 8, queueLimit = 4;
  using Block = std::array<uint8_t, frames * 2>;
  using Json = NativeTransport::Json;

  static uint64_t decode(const uint8_t *bytes, size_t count) {
    uint64_t value = 0;
    while (count-- > 0) value = (value << 8) | *bytes++;
    return value;
  }
  static void encode(uint8_t *bytes, size_t count, uint64_t value) {
    while (count > 0) {
      bytes[--count] = uint8_t(value);
      value >>= 8;
    }
  }
  static bool readAll(SOCKET socket, uint8_t *bytes, size_t size) {
    while (size > 0) {
      int n = NativeTransport::read(socket, bytes, size);
      if (n <= 0) return false;
      bytes += n;
      size -= size_t(n);
    }
    return true;
  }
  static bool writeAll(SOCKET socket, const uint8_t *bytes, size_t size) {
    while (size > 0) {
      int n = NativeTransport::write(socket, bytes, size);
      if (n <= 0) return false;
      bytes += n;
      size -= size_t(n);
    }
    return true;
  }

  struct Connection {
    SOCKET socket;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> stopped{false}, active{false};
    std::deque<Block> blocks;
    std::deque<uint64_t> awaiting;
    std::string error;
    explicit Connection(int family) : socket(::socket(family, SOCK_STREAM, IPPROTO_TCP)) {}
    ~Connection() {
      if (socket != INVALID_SOCKET) {
        NativeTransport::detach(socket);
        closesocket(socket);
      }
      if (stopEvent) CloseHandle(stopEvent);
    }
    void stop(std::string reason = {}) {
      std::lock_guard lock(mutex);
      if (stopped.exchange(true)) return;
      error = std::move(reason);
      active = false;
      blocks.clear();
      if (stopEvent) SetEvent(stopEvent);
      if (socket != INVALID_SOCKET) shutdown(socket, SD_BOTH);
      changed.notify_all();
    }
    void enqueue(const Block &block) {
      std::lock_guard lock(mutex);
      if (stopped) return;
      if (blocks.size() == queueLimit) blocks.pop_front();
      blocks.push_back(block);
      changed.notify_one();
    }
    Status status() {
      std::lock_guard lock(mutex);
      return {!stopped && active, error};
    }
    bool open(const Lease &lease) {
      if (socket == INVALID_SOCKET || !stopEvent || stopped) return false;
      u_long nonblocking = 1;
      if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) return false;
      if (connect(socket, reinterpret_cast<const sockaddr *>(&lease.address), lease.addressSize) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        auto deadline = GetTickCount64() + 3000;
        for (;;) {
          if (stopped || GetTickCount64() >= deadline) return false;
          fd_set writable, failed;
          FD_ZERO(&writable); FD_SET(socket, &writable);
          FD_ZERO(&failed); FD_SET(socket, &failed);
          timeval timeout{0, 50000};
          int result = select(0, nullptr, &writable, &failed, &timeout);
          if (result < 0 || FD_ISSET(socket, &failed)) return false;
          if (FD_ISSET(socket, &writable)) break;
        }
      }
      nonblocking = 0;
      if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0 || stopped) return false;
      int noDelay = 1;
      DWORD timeout = 2500;
      setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
      setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
      NativeTransport::attach(socket, lease.host);
      setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
      const auto hello = Json({{"type","hello"},{"version",3},{"token",lease.token},
        {"channel","microphone"},{"media_key",lease.key},{"request_id",lease.request}}).dump();
      std::vector<uint8_t> wire(5 + hello.size());
      encode(wire.data(), 4, hello.size() + 1);
      std::memcpy(wire.data() + 5, hello.data(), hello.size());
      if (stopped || !writeAll(socket, wire.data(), wire.size())) return false;
      uint8_t header[4];
      if (!readAll(socket, header, sizeof(header))) return false;
      auto size = decode(header, sizeof(header));
      if (size < 2 || size > 256) return false;
      std::vector<uint8_t> body(size);
      if (!readAll(socket, body.data(), body.size()) || body[0] != 0) return false;
      auto ready = Json::parse(body.begin() + 1, body.end(), nullptr, false);
      return ready.is_object() && ready.value("type", "") == "microphone_ready" && !stopped;
    }
    void acknowledge() {
      std::array<uint8_t, 4 + 1 + 8> packet{};
      while (!stopped && readAll(socket, packet.data(), packet.size())) {
        if (decode(packet.data(), 4) != 9 || packet[4] != kind) break;
        auto sequence = decode(packet.data() + 5, 8);
        std::lock_guard lock(mutex);
        if (awaiting.empty() || awaiting.front() != sequence) break;
        awaiting.pop_front();
        changed.notify_one();
      }
      stop("Microphone channel disconnected");
    }
    void send() {
      auto start = std::chrono::steady_clock::now();
      uint64_t sequence = 0;
      std::array<uint8_t, 5 + 28 + Block{}.size()> packet{};
      encode(packet.data(), 4, packet.size() - 4);
      packet[4] = kind;
      encode(packet.data() + 5, 4, 48000);
      encode(packet.data() + 9, 2, 1);
      encode(packet.data() + 11, 2, 16);
      encode(packet.data() + 13, 4, frames);
      for (;;) {
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, [&] { return stopped || (!blocks.empty() && awaiting.size() < credits); });
          if (stopped) break;
          std::memcpy(packet.data() + 33, blocks.front().data(), blocks.front().size());
          blocks.pop_front();
          awaiting.push_back(++sequence);
        }
        encode(packet.data() + 17, 8, sequence);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);
        encode(packet.data() + 25, 8, uint64_t(elapsed.count()));
        if (stopped || !writeAll(socket, packet.data(), packet.size())) break;
      }
      stop("Microphone channel disconnected");
    }
    void capture() {
      const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      HRESULT hr = initialized;
      HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      {
        using Microsoft::WRL::ComPtr;
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> input;
        if (SUCCEEDED(hr) && !ready) hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr))
          hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices));
        if (SUCCEEDED(hr)) hr = devices->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                                reinterpret_cast<void **>(client.GetAddressOf()));
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = 96000;
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
            300000, 0, &format, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&input));
        bool started = false;
        if (SUCCEEDED(hr) && !stopped) {
          hr = client->Start();
          started = SUCCEEDED(hr);
        }
        if (started) {
          active = !stopped;
          Block block{};
          size_t filled = 0;
          HANDLE events[] = {stopEvent, ready};
          while (!stopped) {
            auto event = WaitForMultipleObjects(2, events, FALSE, 1000);
            if (event == WAIT_OBJECT_0) break;
            if (event == WAIT_FAILED) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
            UINT32 samples = 0;
            while (!stopped && SUCCEEDED(hr = input->GetNextPacketSize(&samples)) && samples > 0) {
              BYTE *bytes = nullptr;
              DWORD flags = 0;
              if (FAILED(hr = input->GetBuffer(&bytes, &samples, &flags, nullptr, nullptr))) break;
              if (!samples) break;
              if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) filled = 0;
              size_t offset = 0, total = size_t(samples) * 2;
              while (offset < total && !stopped) {
                size_t length = std::min(block.size() - filled, total - offset);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !bytes)
                  std::memset(block.data() + filled, 0, length);
                else
                  std::memcpy(block.data() + filled, bytes + offset, length);
                offset += length;
                filled += length;
                if (filled == block.size()) {
                  enqueue(block);
                  filled = 0;
                }
              }
              hr = input->ReleaseBuffer(samples);
              if (FAILED(hr)) break;
            }
            if (FAILED(hr)) break;
          }
          client->Stop();
        }
      }
      if (ready) CloseHandle(ready);
      if (SUCCEEDED(initialized)) CoUninitialize();
      stop("Default Windows microphone unavailable: WASAPI " + std::to_string(unsigned(hr)));
    }
    void run(const Lease &lease) {
      std::thread reader, captureWorker;
      try {
        if (!open(lease)) {
          stop("Could not connect to the Linux microphone input");
          return;
        }
        reader = std::thread([this] { acknowledge(); });
        captureWorker = std::thread([this] { capture(); });
        send();
      } catch (const std::exception &) { stop("Could not start microphone forwarding"); }
      if (captureWorker.joinable()) captureWorker.join();
      if (reader.joinable()) reader.join();
    }
  };

  std::mutex mutex;
  std::condition_variable changed;
  std::optional<Lease> lease;
  std::shared_ptr<Connection> current;
  std::thread worker;
  bool quitting = false;
  std::string error;
  void loop() {
    std::unique_lock lock(mutex);
    while (!quitting) {
      changed.wait(lock, [&] { return quitting || lease.has_value(); });
      if (quitting) break;
      auto target = *lease;
      auto connection = std::make_shared<Connection>(target.address.ss_family);
      current = connection;
      lock.unlock();
      connection->run(target);
      lock.lock();
      if (lease && lease->request == target.request) error = connection->status().error;
      current.reset();
      connection.reset();
      changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return quitting || !lease || lease->request != target.request;
      });
    }
  }

public:
  void configure(std::optional<Lease> next) {
    std::lock_guard lock(mutex);
    if (quitting) return;
    if (current) current->stop();
    lease = std::move(next);
    error.clear();
    if (lease && !worker.joinable()) worker = std::thread([this] { loop(); });
    changed.notify_one();
  }
  Status status() {
    std::lock_guard lock(mutex);
    if (!lease) return {};
    auto status = current ? current->status() : Status{};
    if (status.error.empty() && !status.active) status.error = error;
    return status;
  }
  void stop() {
    {
      std::lock_guard lock(mutex);
      quitting = true;
      if (current) current->stop();
      changed.notify_one();
    }
    if (worker.joinable()) worker.join();
  }
  ~MicrophoneForwarder() { stop(); }
};
