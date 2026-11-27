#pragma once
#include <chrono>

// A window submits at most one frame per display period in steady state.
// A late scheduler wake or an idle window starts a fresh period.
class PresentationCadence {
public:
  using Clock = std::chrono::steady_clock;

private:
  Clock::duration period{};
  Clock::time_point next{};

public:
  void refreshRate(unsigned hz) {
    period = hz > 1 && hz <= 1000 ? std::chrono::nanoseconds(1'000'000'000 / hz)
                                : Clock::duration::zero();
    next = {};
  }
  Clock::time_point deadline() const { return next; }
  void submitted(Clock::time_point now) {
    if (period == Clock::duration::zero()) return;
    if (next == Clock::time_point{} || now < next || now - next >= period / 2)
      next = now + period;
    else
      next += period;
  }
};

#ifdef _WIN32
#include <windows.h>

// Wake the STA to submit frames. It can continue handling input and window
// messages while each window is waiting for its own display deadline.
class PresentationWake {
  HANDLE timer = nullptr;
  PTP_WAIT wait = nullptr;
  HWND window = nullptr;
  UINT message = 0;

  static void CALLBACK ready(PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT,
                             TP_WAIT_RESULT) {
    auto *wake = static_cast<PresentationWake *>(context);
    PostMessageW(wake->window, wake->message, 0, 0);
  }

public:
  PresentationWake() = default;
  PresentationWake(const PresentationWake &) = delete;
  PresentationWake &operator=(const PresentationWake &) = delete;
  ~PresentationWake() { stop(); }

  void start(HWND target, UINT notification) {
    window = target;
    message = notification;
    constexpr DWORD HighResolution = 0x2; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    timer = CreateWaitableTimerExW(nullptr, nullptr, HighResolution, TIMER_ALL_ACCESS);
    if (timer)
      wait = CreateThreadpoolWait(ready, this, nullptr);
    if (!wait) stop();
  }
  bool available() const { return wait != nullptr; }
  bool arm(PresentationCadence::Clock::time_point deadline) {
    if (!wait) return false;
    using Ticks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
    const auto ticks = std::chrono::ceil<Ticks>(deadline - PresentationCadence::Clock::now());
    LARGE_INTEGER due{};
    due.QuadPart = ticks.count() > 0 ? -ticks.count() : -1;
    if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      return false;
    SetThreadpoolWait(wait, timer, nullptr);
    return true;
  }
  void stop() {
    if (wait) {
      SetThreadpoolWait(wait, nullptr, nullptr);
      WaitForThreadpoolWaitCallbacks(wait, TRUE);
      CloseThreadpoolWait(wait);
      wait = nullptr;
    }
    if (timer) {
      CloseHandle(timer);
      timer = nullptr;
    }
  }
};
#endif
