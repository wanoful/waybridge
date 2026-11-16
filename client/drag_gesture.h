#pragma once
#include <atomic>
#include <future>
#include <thread>
// OLE may remarshal one source as different IDataObject identities per HWND.
// Observe physical press boundaries independently of those native wrappers.
// The dedicated hook thread only updates a counter and immediately chains;
// it never reads payloads, suppresses input or waits on the controller STA.
namespace NativeDragGesture {
class Observer {
  std::thread worker;
  std::atomic<DWORD> threadId{0};
  std::atomic<bool> stopping{false}, available{false};
  std::atomic<uint64_t> transition{0};
  inline static thread_local Observer *instance = nullptr;
  static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && instance && (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)) {
      auto previous = instance->transition.load();
      instance->transition.store(((previous + 2) & ~uint64_t(1)) |
                                 uint64_t(message == WM_LBUTTONDOWN));
    }
    return CallNextHookEx(nullptr, code, message, data);
  }

public:
  ~Observer() { stop(); }
  void start() {
    if (worker.joinable())
      return;
    stopping = false;
    std::promise<void> ready;
    auto initialized = ready.get_future();
    worker = std::thread([this, ready = std::move(ready)]() mutable {
      instance = this;
      MSG message{};
      PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
      threadId = GetCurrentThreadId();
      HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouse, GetModuleHandleW(nullptr), 0);
      // The first target can be entered halfway through an existing press.
      transition = ((transition.load() + 2) & ~uint64_t(1)) |
                   uint64_t((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
      available = hook != nullptr;
      ready.set_value();
      while (hook && !stopping && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (hook)
        UnhookWindowsHookEx(hook);
      available = false;
      threadId = 0;
      instance = nullptr;
    });
    if (initialized.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
      stop();
  }
  void stop() {
    stopping = true;
    if (auto id = threadId.load())
      PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (worker.joinable())
      worker.join();
  }
  uint64_t press() const {
    auto value = transition.load();
    return available && (value & 1) && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
                   transition.load() == value
               ? value
               : 0;
  }
};
inline Observer observer;
} // namespace NativeDragGesture
