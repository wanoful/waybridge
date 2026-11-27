#include "../client/presentation_cadence.h"
#include <cstdlib>
#include <iostream>

using namespace std::chrono_literals;
using Clock = PresentationCadence::Clock;

static void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

int main() {
  PresentationCadence sixty, fast;
  sixty.refreshRate(60);
  fast.refreshRate(120);
  const auto start = Clock::time_point{} + 10s;
  sixty.submitted(start);
  fast.submitted(start);
  check(sixty.deadline() == start + 16666666ns, "60 Hz period");
  check(fast.deadline() == start + 8333333ns, "120 Hz period");

  auto fastDeadline = fast.deadline();
  fast.submitted(fastDeadline + 1ms);
  check(fast.deadline() == fastDeadline + 8333333ns, "minor timer jitter must not accumulate");
  check(sixty.deadline() == start + 16666666ns, "a foreground deadline changed another window");

  const auto idleResume = start + 900ms;
  fast.submitted(idleResume);
  check(fast.deadline() == idleResume + 8333333ns, "idle window must restart without a burst");
  fast.refreshRate(60);
  check(fast.deadline() == Clock::time_point{}, "monitor change should allow immediate content");
  fast.submitted(idleResume);
  check(fast.deadline() == idleResume + 16666666ns, "new monitor rate did not take effect");
  fast.refreshRate(0);
  fast.submitted(start);
  check(fast.deadline() == Clock::time_point{}, "unknown monitor rate should submit directly");
  std::cout << "PASS: independent 60 and 120 Hz presentation deadlines, jitter, idle and monitor changes\n";

#ifdef _WIN32
  constexpr UINT PresentReady = WM_APP + 4;
  HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  check(window != nullptr, "message-only window");
  PresentationWake wake;
  wake.start(window, PresentReady);
  if (wake.available()) {
    const auto due = Clock::now() + 20ms;
    check(wake.arm(due), "high-resolution timer did not arm");
    check(MsgWaitForMultipleObjects(0, nullptr, FALSE, 2000, QS_POSTMESSAGE) == WAIT_OBJECT_0,
          "presentation timer failed to wake the STA");
    MSG msg{};
    check(PeekMessageW(&msg, window, PresentReady, PresentReady, PM_REMOVE) != 0,
          "presentation timer did not post its message");
    check(Clock::now() >= due - 1ms, "presentation timer fired too early");
    std::cout << "PASS: Windows waitable timer posted a presentation wake\n";
  } else {
    std::cout << "SKIP: host lacks a high-resolution waitable timer\n";
  }
  wake.stop();
  DestroyWindow(window);
#endif
}
