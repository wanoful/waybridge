#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>

// Both ends must use overlapped handles: a synchronous server would itself
// serialize the harness's reader and writer on real Windows.
class WindowsPipeFixture {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE readEvent = nullptr, writeEvent = nullptr, stopEvent = nullptr;
  bool transfer(bool write, void *bytes, DWORD length, DWORD &count, DWORD timeout) {
    OVERLAPPED operation{};
    operation.hEvent = write ? writeEvent : readEvent;
    ResetEvent(operation.hEvent);
    count = 0;
    BOOL ready = write ? WriteFile(pipe, bytes, length, &count, &operation)
                       : ReadFile(pipe, bytes, length, &count, &operation);
    if (ready) return count != 0;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE events[] = {stopEvent, operation.hEvent};
    if (WaitForMultipleObjects(2, events, FALSE, timeout) != WAIT_OBJECT_0 + 1) {
      CancelIoEx(pipe, &operation);
      GetOverlappedResult(pipe, &operation, &count, TRUE);
      return false;
    }
    return GetOverlappedResult(pipe, &operation, &count, FALSE) && count != 0;
  }
public:
  std::wstring name;
  explicit WindowsPipeFixture(DWORD bufferSize = 65536) {
    static LONG sequence = 0;
    name = L"waybridge-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(InterlockedIncrement(&sequence));
    readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pipe = CreateNamedPipeW((L"\\\\.\\pipe\\" + name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, bufferSize, bufferSize, 10000, nullptr);
    if (pipe == INVALID_HANDLE_VALUE || !readEvent || !writeEvent || !stopEvent)
      throw std::runtime_error("Cannot create test pipe");
  }
  WindowsPipeFixture(const WindowsPipeFixture &) = delete;
  WindowsPipeFixture &operator=(const WindowsPipeFixture &) = delete;
  bool accept(DWORD timeout = 15000) {
    OVERLAPPED operation{}; operation.hEvent = readEvent;
    ResetEvent(readEvent);
    if (ConnectNamedPipe(pipe, &operation)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    HANDLE events[] = {stopEvent, readEvent};
    DWORD count;
    if (WaitForMultipleObjects(2, events, FALSE, timeout) != WAIT_OBJECT_0 + 1) {
      CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &count, TRUE); return false;
    }
    return GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE;
  }
  bool read(void *bytes, DWORD length, DWORD &count, DWORD timeout = INFINITE) {
    return transfer(false, bytes, length, count, timeout);
  }
  bool write(std::string bytes, DWORD timeout = 10000) {
    size_t position = 0;
    ULONGLONG deadline = GetTickCount64() + timeout;
    while (position < bytes.size()) {
      ULONGLONG now = GetTickCount64();
      if (now >= deadline) return false;
      DWORD count;
      if (!transfer(true, bytes.data() + position, DWORD(bytes.size() - position), count, DWORD(deadline - now))) return false;
      position += count;
    }
    return true;
  }
  bool readLine(std::string &line, DWORD timeout = 3000) {
    line.clear(); ULONGLONG deadline = GetTickCount64() + timeout;
    for (;;) {
      ULONGLONG now = GetTickCount64();
      if (now >= deadline || line.size() > 1024 * 1024) return false;
      char byte; DWORD count;
      if (!read(&byte, 1, count, DWORD(deadline - now))) return false;
      if (byte == '\n') return true;
      line.push_back(byte);
    }
  }
  void cancel() {
    SetEvent(stopEvent); CancelIoEx(pipe, nullptr);
  }
  // The owner joins fixture users before destroying the pipe/events.
  ~WindowsPipeFixture() {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    for (HANDLE event : {readEvent, writeEvent, stopEvent}) if (event) CloseHandle(event);
  }
};
