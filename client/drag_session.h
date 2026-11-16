#pragma once
#include <functional>
// Platform-independent negotiation facts, with a native STA wait adapter.
namespace DragSession {
enum class Decision { Pending, Accepted, Rejected };
enum class Phase { Idle, Hover, Dropping, Completed, Cancelled };
struct State {
  uint64_t id = 0, target = 0;
  Decision decision = Decision::Pending;
  Phase phase = Phase::Idle;
  bool success = false;
  void begin(uint64_t gesture, uint64_t epoch) {
    id = gesture;
    target = epoch;
    decision = Decision::Pending;
    phase = Phase::Hover;
    success = false;
  }
  bool matches(uint64_t gesture, uint64_t epoch) const {
    return id == gesture && target == epoch && id != 0;
  }
  bool accepted() const { return phase == Phase::Hover && decision == Decision::Accepted; }
  bool terminal() const { return phase == Phase::Completed || phase == Phase::Cancelled; }
  void finish(bool result) {
    success = result;
    phase = result ? Phase::Completed : Phase::Cancelled;
  }
};
// OLE's caller cannot interpret Pending. Keep the STA responsive while waiting
// for the remote target, with identity/lifetime revalidation after every pump.
inline bool wait(const std::function<bool()> &ready, const std::function<bool()> &current,
                 const std::function<void()> &pump, DWORD budget) {
  ULONGLONG end = GetTickCount64() + budget;
  while (current()) {
    pump();
    if (!current())
      return false;
    if (ready())
      return true;
    auto now = GetTickCount64();
    if (now >= end)
      return false;
    DWORD remaining = DWORD((std::min)(ULONGLONG(20), end - now));
    MsgWaitForMultipleObjectsEx(0, nullptr, remaining, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
  }
  return false;
}
} // namespace DragSession
