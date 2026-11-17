#pragma once
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>

// Controller-thread adapter. Physical contact identity survives a compositor
// interaction; only the caller applies HWND geometry or chooses a drop target.
class NativeTouch {
public:
  using Json = nlohmann::json;
  struct Target {
    HWND hwnd = nullptr;
    uint32_t id = 0;
    int width = 0, height = 0, originX = 0, originY = 0;
    RECT image{};
    bool enabled = false;
    uint32_t targetId = 0;
    HWND rootWindow = nullptr;
  };
  struct InteractionSample {
    uint64_t operation = 0, gesture = 0, generation = 0;
    uint32_t id = 0, targetId = 0, slot = 0, edge = 0;
    UINT pointerId = 0;
    std::string kind, phase;
    HWND hwnd = nullptr, rootWindow = nullptr;
    POINT screen{}, initialScreen{};
    RECT initialWindow{};
    bool external = false;
  };
private:
  struct Contact {
    uint32_t slot;
    Target target;
    POINT initialScreen, screen;
    RECT initialWindow;
  };
  std::function<void(const Json &)> emit;
  std::function<void(const InteractionSample &)> interactionCallback;
  bool negotiated = false, requested = true;
  uint32_t leaseId = 0;
  uint64_t generation = 0, lastGeneration = 0, pendingStarted = 0, suppressUntil = 0;
  uint64_t gesture = 0, lastGesture = 0, lastOperation = 0;
  Target owner;
  HWND suppressedWindow = nullptr;
  std::map<UINT, Contact> contacts;
  std::deque<Json> pending;
  std::optional<InteractionSample> interaction;
  bool sent = false;
  static HWND root(const Target &target) { return target.rootWindow ? target.rootWindow : target.hwnd; }
  static uint32_t destination(const Target &target) { return target.targetId ? target.targetId : target.id; }
  static bool focused(const Target &target) {
    HWND focus = GetFocus(), window = root(target);
    return focus == target.hwnd || focus == window || (focus && window && IsChild(window,focus));
  }
  static bool pointerMessage(UINT message) {
    return message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP ||
           message == WM_POINTERCAPTURECHANGED;
  }
  void transmit(Json message) {
    message["generation"] = generation;
    if (emit) emit(message);
    sent = true;
  }
  void packet(const Target &target, Json point) {
    Json message = {{"type","touch"},{"id",target.id},{"target",destination(target)},
                    {"gesture",gesture},{"origin_x",target.originX},{"origin_y",target.originY},
                    {"points",Json::array({std::move(point)})}};
    if (leaseId == target.id && generation) transmit(std::move(message));
    else {
      if (!pendingStarted) pendingStarted = GetTickCount64();
      if (pending.size() >= 64 || GetTickCount64() - pendingStarted > 500) { cancel(); return; }
      pending.push_back(std::move(message));
    }
  }
  void clearContacts() {
    contacts.clear(); pending.clear(); pendingStarted = 0; sent = false;
    if (owner.hwnd) { suppressedWindow = owner.hwnd; suppressUntil = GetTickCount64()+500; }
    owner = {}; gesture = 0;
  }
  void report(const InteractionSample &sample) {
    // Pass a copy: callbacks may synchronously cause capture/focus messages.
    if (interactionCallback) interactionCallback(sample);
  }
  bool interactionSample(UINT message, UINT pointer, POINT screen, bool canceled) {
    if (!interaction) return false;
    if (interaction->pointerId != pointer) {
      // A second finger cannot complete or cancel the owning finger's operation.
      if (message == WM_POINTERUP || message == WM_POINTERCAPTURECHANGED || canceled) contacts.erase(pointer);
      return true;
    }
    if (message == WM_POINTERDOWN) return true;
    if (message == WM_POINTERCAPTURECHANGED && interaction->external && !canceled) return true;
    if (!contacts.count(pointer)) return true;
    InteractionSample sample = *interaction;
    sample.screen = screen;
    sample.phase = message == WM_POINTERCAPTURECHANGED || canceled ? "cancel" :
                   message == WM_POINTERUP ? "up" : "motion";
    *interaction = sample;
    contacts.at(pointer).screen = screen;
    suppressUntil = GetTickCount64()+500;
    if (sample.phase == "cancel" || sample.phase == "up") {
      // External OLE completion follows physical UP, so retain its operation
      // token but never retain a physically held pointer after that UP.
      bool retain = sample.external && sample.phase == "up";
      if (!retain) interaction.reset();
      clearContacts();
    }
    report(sample);
    return true;
  }
public:
  explicit NativeTouch(std::function<void(const Json &)> sender = {}) : emit(std::move(sender)) {}
  void onInteraction(std::function<void(const InteractionSample &)> callback) { interactionCallback = std::move(callback); }
  bool ownsInteraction(HWND window = nullptr) const {
    return interaction && (!window || interaction->hwnd == window || interaction->rootWindow == window);
  }
  bool ownsPointer(UINT pointer) const { return contacts.count(pointer) != 0; }
  std::optional<InteractionSample> activeInteraction() const { return interaction; }
  bool externalInteraction(uint64_t operation, bool external) {
    if (!interaction || interaction->operation != operation ||
        (!external && !contacts.count(interaction->pointerId))) return false;
    interaction->external = external;
    return true;
  }
  bool beginInteraction(const Json &message, HWND sourceRoot) {
    uint64_t operation = message.value("operation",uint64_t(0));
    auto ack = [&](bool accepted) {
      if (emit) emit({{"type","touch_interaction_ack"},{"operation",operation},{"accepted",accepted}});
      return accepted;
    };
    if (interaction && interaction->operation == operation) {
      const auto &active = *interaction;
      return ack(contacts.count(active.pointerId) && sourceRoot == active.rootWindow &&
                 message.value("gesture",uint64_t(0)) == active.gesture &&
                 message.value("generation",uint64_t(0)) == active.generation &&
                 message.value("id",uint32_t(0)) == active.id &&
                 message.value("slot",uint32_t(10)) == active.slot &&
                 message.value("kind",std::string()) == active.kind);
    }
    std::string kind = message.value("kind",std::string());
    uint64_t requestedGesture = message.value("gesture",uint64_t(0));
    uint32_t slot = message.value("slot",uint32_t(10));
    auto contact = std::find_if(contacts.begin(),contacts.end(),[&](const auto &entry) { return entry.second.slot == slot; });
    bool valid = negotiated && requested && !interaction && operation && operation > lastOperation &&
                 (kind == "move" || kind == "resize" || kind == "dnd") && sent && pending.empty() &&
                 requestedGesture && requestedGesture == gesture && generation &&
                 message.value("generation",uint64_t(0)) == generation &&
                 message.value("id",uint32_t(0)) == leaseId && leaseId == owner.id &&
                 contact != contacts.end() && IsWindow(contact->second.target.hwnd) &&
                 IsWindow(root(contact->second.target)) && sourceRoot == root(contact->second.target);
    if (!valid) return ack(false);
    const auto &held = contact->second;
    InteractionSample sample;
    sample.operation = operation; sample.gesture = gesture; sample.generation = generation;
    sample.id = owner.id; sample.targetId = destination(held.target); sample.slot = slot;
    sample.edge = message.value("edge",uint32_t(0)); sample.pointerId = contact->first;
    sample.kind = kind; sample.phase = "begin"; sample.hwnd = held.target.hwnd;
    sample.rootWindow = root(held.target); sample.screen = held.screen;
    sample.initialScreen = held.initialScreen; sample.initialWindow = held.initialWindow;
    interaction = sample; lastOperation = operation;
    ack(true); // The server must know ownership before any operation input.
    if (interaction && interaction->operation == operation) report(sample);
    return true;
  }
  bool endInteraction(uint64_t operation) {
    if (!interaction || interaction->operation != operation) return false;
    InteractionSample sample = *interaction;
    sample.phase = "end";
    interaction.reset(); clearContacts();
    report(sample);
    return true;
  }
  void hello(bool enabled) {
    cancel(); negotiated = enabled; lastGeneration = generation = 0; leaseId = 0;
    lastGesture = lastOperation = 0;
  }
  void enabled(bool value) {
    if (requested == value) return;
    cancel(); requested = value;
    if (!value) { suppressedWindow = nullptr; suppressUntil = 0; }
  }
  void state(const Json &message) {
    uint64_t next = message.value("generation",uint64_t(0));
    if (!negotiated || !next || next <= lastGeneration) return;
    if (!requested) { cancel(nullptr,false); lastGeneration = next; return; }
    bool enabled = message.value("enabled",false);
    uint32_t id = enabled ? message.value("id",uint32_t(0)) : 0;
    bool acquiring = !enabled && owner.id && !pending.empty() &&
                     message.value("pending_target",uint32_t(0)) == owner.id &&
                     IsWindow(owner.hwnd) && focused(owner) && GetTickCount64()-pendingStarted <= 500;
    // Only the explicit initial focus handoff retains queued input. Any real
    // lease revocation ends an interaction without echoing a second cancel.
    if ((sent && (id != leaseId || next != generation)) ||
        (interaction && (id != leaseId || next != generation)) ||
        (!pending.empty() && ((!enabled && !acquiring) || (enabled && id != owner.id))))
      cancel(nullptr,false);
    lastGeneration = generation = next;
    leaseId = id;
    if (!pending.empty() && id == owner.id) {
      if (!IsWindow(owner.hwnd) || !focused(owner) || GetTickCount64()-pendingStarted > 500) { cancel(); return; }
      while (!pending.empty()) {
        auto queued = std::move(pending.front()); pending.pop_front(); transmit(std::move(queued));
      }
      pendingStarted = 0;
      if (contacts.empty()) { owner = {}; sent = false; gesture = 0; }
    }
  }
  void cancel(HWND window = nullptr, bool notify = true) {
    if (window && owner.hwnd != window && root(owner) != window && !ownsInteraction(window) &&
        std::none_of(contacts.begin(),contacts.end(),[&](const auto &entry) { return entry.second.target.hwnd == window; })) return;
    auto active = interaction;
    interaction.reset();
    bool ordinaryCancel = !active && notify && sent && leaseId && generation;
    Json message = {{"type","touch"},{"id",leaseId},{"generation",generation},{"gesture",gesture},{"action","cancel"}};
    clearContacts(); leaseId = 0; generation = 0;
    if (ordinaryCancel && emit) emit(message);
    if (active) { active->phase = notify ? "cancel" : "end"; report(*active); }
  }
  // The standalone probe supplies controlled OS samples here; production uses
  // exactly the same identity, geometry, lease and cancellation state machine.
  bool sample(const Target &target, UINT message, UINT pointer, POINT screen, bool canceled = false) {
    if (!pointerMessage(message)) return false;
    if (interactionSample(message,pointer,screen,canceled)) return true;
    if (!negotiated || !requested || !target.enabled) return false;
    if (message == WM_POINTERCAPTURECHANGED || canceled) {
      if (contacts.count(pointer)) cancel();
      return true;
    }
    if (message == WM_POINTERDOWN) {
      if (!IsWindow(target.hwnd) || !IsWindow(root(target)) || !focused(target) || target.width <= 0 || target.height <= 0 ||
          target.image.right <= target.image.left || target.image.bottom <= target.image.top) return true;
      if (owner.hwnd && (owner.id != target.id || root(owner) != root(target))) cancel();
      if (contacts.count(pointer) || contacts.size() >= 10) return true;
      POINT local = screen; ScreenToClient(target.hwnd,&local);
      if (!PtInRect(&target.image,local)) return true;
      if (!owner.hwnd) { owner = target; sent = false; }
      if (contacts.empty()) gesture = ++lastGesture;
      uint32_t slot = 0;
      while (std::any_of(contacts.begin(),contacts.end(),[&](const auto &entry) { return entry.second.slot == slot; })) ++slot;
      RECT initialWindow{}; GetWindowRect(root(target),&initialWindow);
      contacts.emplace(pointer,Contact{slot,target,screen,screen,initialWindow});
      suppressedWindow = target.hwnd; suppressUntil = GetTickCount64()+500;
    }
    auto current = contacts.find(pointer);
    if (current == contacts.end() || current->second.target.hwnd != target.hwnd || owner.id != target.id ||
        root(current->second.target) != root(target) ||
        destination(current->second.target) != destination(target)) return true;
    if (target.image.right <= target.image.left || target.image.bottom <= target.image.top) { cancel(); return true; }
    current->second.target = target; current->second.screen = screen;
    POINT local = screen; ScreenToClient(target.hwnd,&local);
    const char *phase = message == WM_POINTERDOWN ? "down" : message == WM_POINTERUP ? "up" : "motion";
    Json point = {{"slot",current->second.slot},{"phase",phase},
                  {"x",double(local.x-target.image.left)*target.width/(target.image.right-target.image.left)},
                  {"y",double(local.y-target.image.top)*target.height/(target.image.bottom-target.image.top)}};
    if (message == WM_POINTERUP) contacts.erase(current);
    packet(target,std::move(point));
    if (contacts.empty() && pending.empty()) { owner = {}; sent = false; gesture = 0; }
    suppressUntil = GetTickCount64()+500;
    return true;
  }
  bool handle(const Target &target, UINT message, WPARAM wp) {
    if (!pointerMessage(message) || (!interaction && (!negotiated || !requested || !target.enabled))) return false;
    UINT pointer = GET_POINTERID_WPARAM(wp);
    if (message == WM_POINTERCAPTURECHANGED) {
      if (!contacts.count(pointer)) return false;
      POINT screen{};
      if (interaction) screen = interaction->screen;
      return sample(target,message,pointer,screen);
    }
    POINTER_INPUT_TYPE type{};
    if (!GetPointerType(pointer,&type) || type != PT_TOUCH) return false;
    POINTER_INFO info{};
    if (!GetPointerInfo(pointer,&info)) {
      if (contacts.count(pointer)) {
        if (interaction) sample(target,WM_POINTERCAPTURECHANGED,pointer,interaction->screen);
        else cancel();
      }
      return true;
    }
    return sample(target,message,pointer,info.ptPixelLocation,(info.pointerFlags&POINTER_FLAG_CANCELED) != 0);
  }
  bool promotedMouse(HWND window, ULONG_PTR extra) const {
    if (interaction && interaction->external) return false;
    // Documented Windows pen/touch signature; bit 7 distinguishes touch from pen.
    return negotiated && requested && (interaction || (suppressedWindow == window && GetTickCount64() <= suppressUntil)) &&
           (extra&0xffffff00) == 0xff515700 && (extra&0x80);
  }
};
