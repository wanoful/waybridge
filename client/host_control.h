#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
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
  HANDLE readEvent = nullptr, writeEvent = nullptr, stopEvent = nullptr;
  HWND target = nullptr;
  std::atomic<bool> stopping{false};
  std::atomic<bool> connected{false};
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> outgoing;
  bool writing = false;
  std::vector<nlohmann::json> incoming;
  std::function<void(const nlohmann::json &)> beforeDispatch;
  std::array<char, 4096> readBuffer{};
  size_t readPosition = 0, readLength = 0;
  static constexpr ULONGLONG NoDeadline = ~ULONGLONG(0);

  // A synchronous pipe handle serializes its read and write calls on Windows.
  // Each direction instead owns an event and at most one overlapped operation.
  // Always reap a cancelled operation before its stack/buffer storage goes away.
  bool transfer(bool write, void *bytes, DWORD length, DWORD &count,
                ULONGLONG deadline = NoDeadline) {
    count = 0;
    if (stopping || (deadline != NoDeadline && GetTickCount64() >= deadline)) return false;
    OVERLAPPED operation{};
    operation.hEvent = write ? writeEvent : readEvent;
    ResetEvent(operation.hEvent);
    BOOL completed = write ? WriteFile(pipe, bytes, length, &count, &operation)
                           : ReadFile(pipe, bytes, length, &count, &operation);
    if (completed) return count != 0;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    DWORD timeout = INFINITE;
    if (deadline != NoDeadline) {
      ULONGLONG now = GetTickCount64();
      timeout = now >= deadline ? 0 : DWORD((std::min)(deadline - now, ULONGLONG(INFINITE - 1)));
    }
    HANDLE events[] = {stopEvent, operation.hEvent};
    DWORD result = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (result != WAIT_OBJECT_0 + 1) {
      CancelIoEx(pipe, &operation);
      GetOverlappedResult(pipe, &operation, &count, TRUE);
      return false;
    }
    return GetOverlappedResult(pipe, &operation, &count, FALSE) && count != 0;
  }
  bool readLine(std::string &line, ULONGLONG deadline = NoDeadline) {
    line.clear();
    while (!stopping) {
      if (readPosition == readLength) {
        DWORD count;
        if (!transfer(false, readBuffer.data(), DWORD(readBuffer.size()), count, deadline)) return false;
        readPosition = 0; readLength = count;
      }
      char ch = readBuffer[readPosition++];
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
        if (self->beforeDispatch) self->beforeDispatch(value);
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
        self->writing = true;
      }
      char *data = line.data(); size_t left = line.size(); DWORD count;
      bool failed = false;
      ULONGLONG deadline = GetTickCount64() + 10000;
      while (left && !self->stopping) {
        if (!self->transfer(true, data, DWORD(left), count, deadline)) {
          failed = true; break;
        }
        data += count; left -= count;
      }
      {
        std::lock_guard<std::mutex> guard(self->mutex);
        self->writing = false;
      }
      self->wake.notify_all();
      if (failed) {
        if (!self->stopping) PostMessageW(self->target, WM_CLOSE, 0, 0);
        return 0;
      }
    }
    return 0;
  }
public:
  bool active() const { return connected.load(); }
  nlohmann::json connect(const std::wstring &name) {
    if (pipe != INVALID_HANDLE_VALUE) throw std::runtime_error("GUI control pipe is already connected");
    if (name.empty() || name.size() > 128 ||
        name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::wstring::npos)
      throw std::runtime_error("Invalid control pipe name");
    std::wstring path = L"\\\\.\\pipe\\" + name;
    if (!WaitNamedPipeW(path.c_str(), 15000)) throw std::runtime_error("GUI control pipe is unavailable");
    pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot connect GUI control pipe");
    try {
      stopping = false;
      readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!readEvent || !writeEvent || !stopEvent) throw std::runtime_error("Cannot create GUI control events");
      readPosition = readLength = 0;
      std::string line;
      if (!readLine(line, GetTickCount64() + 15000)) throw std::runtime_error("Missing GUI start request");
      auto value = nlohmann::json::parse(line);
      if (!value.is_object() || value.value("type", "") != "start" || value.value("version", 0) != 1)
        throw std::runtime_error("Incompatible GUI control protocol");
      connected = true;
      return value;
    } catch (...) {
      stop(); throw;
    }
  }
  void start(HWND hwnd, std::function<void(const nlohmann::json &)> prepare = {}) {
    if (!active()) return;
    target = hwnd;
    beforeDispatch = std::move(prepare);
    reader = CreateThread(nullptr, 0, readThread, this, 0, nullptr);
    writer = CreateThread(nullptr, 0, writeThread, this, 0, nullptr);
    if (!reader || !writer) {
      stop(); throw std::runtime_error("Cannot start GUI control workers");
    }
  }
  void emit(const nlohmann::json &value) {
    if (!active() || stopping) return;
    std::string line = value.dump() + "\n";
    if (line.size() > 1024 * 1024) return;
    std::lock_guard<std::mutex> guard(mutex);
    // Telemetry and lifecycle updates must remain bounded even if the UI stops reading.
    if (outgoing.size() >= 128) {
      // Losing a notification close/reset (or a tray lifecycle transition)
      // would retain stale interactive UI. Disconnect an unresponsive host
      // instead; connection teardown invalidates all such entries and actions.
      if (target) PostMessageW(target, WM_CLOSE, 0, 0);
      return;
    }
    outgoing.push_back(std::move(line)); wake.notify_one();
  }
  std::vector<nlohmann::json> take() {
    std::lock_guard<std::mutex> guard(mutex);
    auto result = std::move(incoming); incoming.clear(); return result;
  }
  // Startup failures occur before the message loop. Give their diagnostic event
  // a bounded opportunity to reach the GUI before cancelling the pipe writer.
  void flush(std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) {
    if (!active() || !writer) return;
    std::unique_lock<std::mutex> guard(mutex);
    wake.wait_for(guard, timeout, [&] { return stopping || (outgoing.empty() && !writing); });
  }
  void stop() {
    connected = false;
    stopping = true;
    if (stopEvent) SetEvent(stopEvent);
    wake.notify_all();
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    for (HANDLE thread : {reader, writer}) if (thread) {
      WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
    }
    reader = writer = nullptr;
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE;
    for (HANDLE event : {readEvent, writeEvent, stopEvent}) if (event) CloseHandle(event);
    readEvent = writeEvent = stopEvent = nullptr;
    std::lock_guard<std::mutex> guard(mutex);
    outgoing.clear(); incoming.clear(); writing = false;
  }
  ~HostControl() { stop(); }
};
