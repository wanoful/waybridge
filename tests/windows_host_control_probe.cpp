// Build: x86_64-w64-mingw32-g++ -std=c++17 -static -O2 -Iclient
//        tests/windows_host_control_probe.cpp -o build/tests/windows-host-control-probe.exe
// Run on Windows or under Wine. No compositor, credentials, or network required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <exception>
#include <iostream>
#include <thread>
#include "host_control.h"
#include "windows_pipe_fixture.h"

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void duplexAndCancel() {
  WindowsPipeFixture pipe(1024);
  HostControl control;
  HANDLE drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::exception_ptr peerError;
  std::thread peer([&] {
    try {
      require(pipe.accept(), "Server did not accept client");
      // A command coalesced with bootstrap must survive the handover to reader.
      require(pipe.write("{\"type\":\"start\",\"version\":1}\n{\"type\":\"command\",\"n\":0}\n"), "Bootstrap failed");
      std::string line;
      require(pipe.readLine(line), "Pending native read blocked connected event");
      require(nlohmann::json::parse(line).value("type", "") == "connected", "Missing connected event");
      for (int i = 0; i < 80; ++i) {
        require(pipe.readLine(line), "Status event did not arrive");
        require(nlohmann::json::parse(line).value("n", -1) == i, "Writer reordered events");
      }
      require(pipe.write("{\"type\":\"comm"), "Fragment prefix failed");
      Sleep(30);
      require(pipe.write("and\",\"n\":1}\n{\"type\":\"command\",\"n\":2}\n"), "Fragment suffix failed");
      SetEvent(drained);
      // Keep the peer open without reading: the next native write must block.
      require(WaitForSingleObject(release, 10000) == WAIT_OBJECT_0, "Cancellation test stalled");
    } catch (...) { peerError = std::current_exception(); SetEvent(drained); }
  });
  try {
    require(control.connect(pipe.name).value("version", 0) == 1, "Invalid bootstrap");
    control.start(nullptr);
    Sleep(100); // The reader is now waiting for a new GUI command.
    control.emit({{"type", "connected"}});
    for (int i = 0; i < 80; ++i) control.emit({{"type", "status"}, {"n", i}});
    control.flush(std::chrono::milliseconds(2000));
    require(WaitForSingleObject(drained, 5000) == WAIT_OBJECT_0, "Peer did not drain events");
    int next = 0;
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (next != 3 && GetTickCount64() < deadline) {
      for (const auto &message : control.take())
        require(message.value("n", -1) == next++, "Reader lost or reordered command");
      Sleep(5);
    }
    require(next == 3, "Fragmented/coalesced commands missing");
    control.emit({{"type", "blocked"}, {"payload", std::string(900 * 1024, 'x')}});
    ULONGLONG before = GetTickCount64();
    control.flush(std::chrono::milliseconds(150));
    require(GetTickCount64() - before >= 100, "Test did not create a pending write");
    before = GetTickCount64();
    control.stop();
    require(GetTickCount64() - before < 2000, "Stop did not cancel pending read/write");
    control.stop(); // Shutdown remains idempotent.
  } catch (...) {
    control.stop(); SetEvent(release); pipe.cancel(); peer.join();
    CloseHandle(drained); CloseHandle(release); throw;
  }
  SetEvent(release); pipe.cancel(); peer.join();
  CloseHandle(drained); CloseHandle(release);
  if (peerError) std::rethrow_exception(peerError);
}

static void failedBootstrap(bool partial) {
  WindowsPipeFixture pipe;
  HostControl control;
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread peer([&] {
    if (pipe.accept()) {
      pipe.write(partial ? "{\"type\":" : "{\"type\":\"start\",\"version\":99}\n");
      WaitForSingleObject(release, 20000);
    }
  });
  bool rejected = false;
  ULONGLONG before = GetTickCount64();
  try { control.connect(pipe.name); } catch (const std::exception &) { rejected = true; }
  ULONGLONG elapsed = GetTickCount64() - before;
  control.stop(); SetEvent(release); pipe.cancel(); peer.join(); CloseHandle(release);
  require(rejected && !control.active(), "Invalid/incomplete bootstrap was accepted");
  if (partial) require(elapsed >= 14000 && elapsed < 18000, "Bootstrap deadline did not bound pending read");
}

int main() {
  try {
    duplexAndCancel();
    failedBootstrap(false);
    failedBootstrap(true);
    std::cout << "PASS: pending reader permits ordered connected/status writes, buffered/fragmented commands survive bootstrap, stop cancels pending read/write, invalid bootstrap cleans up and incomplete bootstrap expires\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
