#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "third_party/json.hpp"

// The GUI owns a current-user-only, randomly named pipe. Video never enters it.
// Events use a bounded writer queue so an unresponsive GUI cannot stall rendering.
class HostControl {
public:
  static constexpr UINT Message = WM_APP + 21;
private:
  HANDLE pipe = INVALID_HANDLE_VALUE, reader = nullptr, writer = nullptr;
  HWND target = nullptr;
  std::atomic<bool> stopping{false};
  std::atomic<bool> connected{false};
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> outgoing;
  std::vector<nlohmann::json> incoming;
  bool readLine(std::string &line) {
    char ch; DWORD count;
    line.clear();
    while (!stopping && ReadFile(pipe, &ch, 1, &count, nullptr) && count) {
      if (ch == '\n') return true;
      if (line.size() >= 65536) return false;
      line.push_back(ch);
    }
    return false;
  }
  static DWORD WINAPI readThread(void *context) {
    auto *self = static_cast<HostControl *>(context);
    try {
      std::string line;
      while (self->readLine(line)) {
        auto value = nlohmann::json::parse(line);
        if (!value.is_object()) break;
        {
          std::lock_guard<std::mutex> guard(self->mutex);
          if (self->incoming.size() >= 64) break;
          self->incoming.push_back(std::move(value));
        }
        PostMessageW(self->target, Message, 0, 0);
      }
    } catch (...) {}
    if (!self->stopping) PostMessageW(self->target, WM_CLOSE, 0, 0);
    return 0;
  }
  static DWORD WINAPI writeThread(void *context) {
    auto *self = static_cast<HostControl *>(context);
    while (!self->stopping) {
      std::string line;
      {
        std::unique_lock<std::mutex> guard(self->mutex);
        self->wake.wait(guard, [&] { return self->stopping || !self->outgoing.empty(); });
        if (self->stopping) break;
        line = std::move(self->outgoing.front()); self->outgoing.pop_front();
      }
      const char *data = line.data(); size_t left = line.size(); DWORD count;
      while (left && !self->stopping) {
        if (!WriteFile(self->pipe, data, DWORD(left), &count, nullptr) || !count) {
          PostMessageW(self->target, WM_CLOSE, 0, 0); return 0;
        }
        data += count; left -= count;
      }
    }
    return 0;
  }
public:
  bool active() const { return connected.load(); }
  nlohmann::json connect(const std::wstring &name) {
    if (name.empty() || name.size() > 128 ||
        name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::wstring::npos)
      throw std::runtime_error("Invalid control pipe name");
    std::wstring path = L"\\\\.\\pipe\\" + name;
    if (!WaitNamedPipeW(path.c_str(), 15000)) throw std::runtime_error("GUI control pipe is unavailable");
    pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot connect GUI control pipe");
    connected = true;
    std::string line;
    if (!readLine(line)) throw std::runtime_error("Missing GUI start request");
    auto value = nlohmann::json::parse(line);
    if (!value.is_object() || value.value("type", "") != "start" || value.value("version", 0) != 1)
      throw std::runtime_error("Incompatible GUI control protocol");
    return value;
  }
  void start(HWND hwnd) {
    if (!active()) return;
    target = hwnd;
    reader = CreateThread(nullptr, 0, readThread, this, 0, nullptr);
    writer = CreateThread(nullptr, 0, writeThread, this, 0, nullptr);
    if (!reader || !writer) throw std::runtime_error("Cannot start GUI control workers");
  }
  void emit(const nlohmann::json &value) {
    if (!active() || stopping) return;
    std::string line = value.dump() + "\n";
    if (line.size() > 1024 * 1024) return;
    std::lock_guard<std::mutex> guard(mutex);
    // Telemetry and lifecycle updates must remain bounded even if the UI stops reading.
    if (outgoing.size() >= 128) outgoing.pop_front();
    outgoing.push_back(std::move(line)); wake.notify_one();
  }
  std::vector<nlohmann::json> take() {
    std::lock_guard<std::mutex> guard(mutex);
    auto result = std::move(incoming); incoming.clear(); return result;
  }
  void stop() {
    if (!connected.exchange(false)) return;
    stopping = true; wake.notify_all();
    if (reader) CancelSynchronousIo(reader);
    if (writer) CancelSynchronousIo(writer);
    for (HANDLE thread : {reader, writer}) if (thread) {
      WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
    }
    reader = writer = nullptr; CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE;
  }
  ~HostControl() { stop(); }
};
