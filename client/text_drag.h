#pragma once
// Bounded OLE Unicode text <-> Wayland MIME data. Never creates a file or
// changes the clipboard. All state belongs to the controller STA.
#include "drag_trace.h"
#include "drag_session.h"
#include "drag_gesture.h"
#include "text_offer.h"
namespace TextDrag {
inline uint64_t sequence = 0, targetEpoch = 0, revision = 0;
inline Json lastPosition = Json::object();
inline DragSession::State state;
inline const uint64_t &active = state.id;
inline bool resultNegotiated = false;
inline HWND owner = nullptr;
constexpr UINT_PTR Timer = 0xd0d;
inline std::shared_ptr<NativeTextOffer::Source> source;
inline void releaseSource() {
  auto old = std::move(source);
  old.reset();
  if (!source) {
    if (owner)
      KillTimer(owner, Timer);
    NativeDragGesture::observer.stop();
  }
}
inline void attach(HWND hwnd) { owner = hwnd; }
inline void tick() {
  if (source &&
      ((!active && !NativeDragGesture::observer.press()) || GetTickCount64() > source->expires))
    releaseSource();
}

inline void configure(bool results) { resultNegotiated = results; }

inline bool supports(IDataObject *data) {
  FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  return data && SUCCEEDED(data->QueryGetData(&format));
}
inline void cancel(uint64_t id) {
  if (!id || active != id)
    return;
  sendJson({{"type", "text_drag_cancel"}, {"drag", id}, {"target_epoch", state.target}});
  state = DragSession::State{};
}
// A target lease starts before any external QueryGetData/GetData call. Older
// target callbacks cannot cancel a newer target, or resurrect a pending begin.
inline bool currentTarget(uint64_t token) { return token && token == targetEpoch; }
inline uint64_t enterTarget() {
  cancel(active);
  return ++targetEpoch;
}
inline void leaveTarget(uint64_t token) {
  if (currentTarget(token)) {
    cancel(active);
    ++targetEpoch;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
      releaseSource();
  }
}
inline void reset() {
  cancel(active);
  ++targetEpoch;
  releaseSource();
}
inline uint64_t begin(IDataObject *data, Json position, uint64_t target,
                      const std::function<bool()> &current) {
  if (!currentTarget(target) || !current())
    return 0;
  if (owner)
    NativeDragGesture::observer.start();
  auto press = NativeDragGesture::observer.press();
  bool reuse =
      owner && source && press && press == source->press && GetTickCount64() <= source->expires;
  auto offered =
      reuse ? std::optional<std::string>{source->text}
            : NativeTextOffer::read(data, [&] { return currentTarget(target) && current(); });
  if (!offered || (reuse && NativeDragGesture::observer.press() != press)) {
    if (!source)
      NativeDragGesture::observer.stop();
    return 0;
  }
  uint64_t gesture = reuse ? source->gesture : ++sequence;
  if (owner && !reuse) {
    if (!currentTarget(target) || !current())
      return 0;
    source = std::make_shared<NativeTextOffer::Source>();
    source->text = *offered;
    source->gesture = gesture;
    source->press = press;
    source->expires = GetTickCount64() + 120000;
    if (owner)
      SetTimer(owner, Timer, 35, nullptr);
  }
  const auto &text = *offered;
  cancel(active);
  state.begin(gesture, resultNegotiated ? ++revision : target);
  lastPosition = position;
  position["type"] = "text_drag_enter";
  position["drag"] = active;
  position["target_epoch"] = state.target;
  position["data"] = base64(reinterpret_cast<const uint8_t *>(text.data()), text.size());
  DragTrace::event("text.enter", {{"drag", active},
                                  {"target_epoch", state.target},
                                  {"window", position.value("id", 0u)},
                                  {"bytes", text.size()},
                                  {"source_reused", reuse}});
  sendJson(position);
  return active;
}
inline bool refresh(uint64_t id, Json position, bool force = false) {
  if (!resultNegotiated || !id || active != id || state.phase != DragSession::Phase::Hover)
    return false;
  bool moved = position.value("x", 0.) != lastPosition.value("x", 0.) ||
               position.value("y", 0.) != lastPosition.value("y", 0.) ||
               position.value("id", 0u) != lastPosition.value("id", 0u);
  if (!force && !moved)
    return false;
  state.target = ++revision;
  state.decision = DragSession::Decision::Pending;
  lastPosition = position;
  position["type"] = "text_drag_retarget";
  position["drag"] = id;
  position["target_epoch"] = state.target;
  sendJson(position);
  return true;
}
inline void motion(uint64_t id, Json position) {
  if (!id || active != id || state.phase != DragSession::Phase::Hover)
    return;
  lastPosition = position;
  position["type"] = "text_drag_motion";
  position["drag"] = id;
  position["target_epoch"] = state.target;
  sendJson(position);
}
inline bool allowed(uint64_t id) { return id && active == id && state.accepted(); }
inline bool awaitAcceptance(uint64_t id, uint64_t target, const std::function<bool()> &current,
                            const std::function<void()> &pump) {
  ULONGLONG lastPulse = 0;
  const auto update = [&] {
    pump();
    if (current() && currentTarget(target) && state.id == id &&
        state.phase == DragSession::Phase::Hover &&
        state.decision != DragSession::Decision::Accepted && GetTickCount64() - lastPulse >= 35) {
      lastPulse = GetTickCount64();
      motion(id, lastPosition);
    }
  };
  bool ready = DragSession::wait([] { return state.decision == DragSession::Decision::Accepted; },
                                 [&] {
                                   return current() && currentTarget(target) && state.id == id &&
                                          state.phase == DragSession::Phase::Hover;
                                 },
                                 update, 750);
  if (!ready)
    DragTrace::event("negotiation.timeout", {{"drag", id}, {"target_epoch", target}});
  return ready && allowed(id);
}

inline bool drop(uint64_t id, Json position) {
  if (!allowed(id)) {
    cancel(id);
    return false;
  }
  state.phase = DragSession::Phase::Dropping;
  position["type"] = "text_drag_drop";
  position["drag"] = id;
  position["target_epoch"] = state.target;
  sendJson(position);
  if (!resultNegotiated) {
    state = DragSession::State{};
    ++targetEpoch;
    releaseSource();
  }
  return true;
}
inline bool awaitCompletion(uint64_t id, uint64_t target, const std::function<bool()> &current,
                            const std::function<void()> &pump) {
  if (!resultNegotiated)
    return true;
  bool ready = DragSession::wait(
      [] { return state.terminal(); },
      [&] { return current() && currentTarget(target) && state.id == id; }, pump, 2000);
  bool success = ready && state.success;
  if (state.id == id && currentTarget(target)) {
    if (!success)
      cancel(id);
    else
      state = DragSession::State{};
    ++targetEpoch;
    releaseSource();
  }
  if (!ready)
    DragTrace::event("delivery.unknown", {{"drag", id}});
  return success;
}
inline bool control(const Json &message) {
  auto kind = message.value("type", "");
  if (kind != "text_drag_status" && kind != "text_drag_finished")
    return false;
  auto id = message.value("drag", uint64_t(0));
  auto target = message.value("target_epoch", state.target);
  if (!state.matches(id, target))
    return true;
  if (kind == "text_drag_finished") {
    if (state.phase == DragSession::Phase::Dropping)
      state.finish(message.value("success", false));
  } else if (state.phase == DragSession::Phase::Hover) {
    auto status = message.value("status", "");
    state.decision = message.value("accepted", false) ? DragSession::Decision::Accepted
                     : status == "rejected"           ? DragSession::Decision::Rejected
                                                      : DragSession::Decision::Pending;
  }
  DragTrace::event(kind.c_str(),
                   {{"drag", id}, {"target_epoch", target}, {"accepted", state.accepted()}});
  return true;
}
} // namespace TextDrag
