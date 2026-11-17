#pragma once
#include <windows.h>
#include "third_party/json.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>

// Controller-thread adapter. The wire carries bounded contact batches, not
// promoted mouse clicks. A new focused window waits for the server's touch lease.
class NativeTouch {
public:
  using Json = nlohmann::json;
  struct Target {
    HWND hwnd = nullptr;
    uint32_t id = 0;
    int width = 0, height = 0, originX = 0, originY = 0;
    RECT image{};
    bool enabled = false;
  };
private:
  struct Contact { uint32_t slot; };
  std::function<void(const Json &)> emit;
  bool negotiated = false, requested = true;
  uint32_t leaseId = 0;
  uint64_t generation = 0, lastGeneration = 0, pendingStarted = 0, suppressUntil = 0;
  Target owner;
  HWND suppressedWindow = nullptr;
  std::map<UINT, Contact> contacts;
  std::deque<Json> pending;
  bool sent = false;
  static bool pointerMessage(UINT message) {
    return message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP ||
           message == WM_POINTERCAPTURECHANGED;
  }
  void transmit(Json packet) {
    packet["generation"] = generation;
    if (emit) emit(packet);
    sent = true;
  }
  void packet(Json point) {
    Json message = {{"type","touch"},{"id",owner.id},{"origin_x",owner.originX},
                    {"origin_y",owner.originY},{"points",Json::array({std::move(point)})}};
    if (leaseId == owner.id && generation) transmit(std::move(message));
    else {
      if (!pendingStarted) pendingStarted = GetTickCount64();
      if (pending.size() >= 64 || GetTickCount64() - pendingStarted > 500) {
        cancel(); return;
      }
      pending.push_back(std::move(message));
    }
  }
public:
  explicit NativeTouch(std::function<void(const Json &)> sender = {}) : emit(std::move(sender)) {}
  void hello(bool enabled) {
    cancel(); negotiated = enabled; lastGeneration = generation = 0; leaseId = 0;
  }
  void enabled(bool value) {
    if (requested == value) return;
    cancel(); requested = value;
    if (!value) { suppressedWindow = nullptr; suppressUntil = 0; }
  }
  void state(const Json &message) {
    uint64_t next = message.value("generation",uint64_t(0));
    if (!negotiated || !next || next <= lastGeneration) return;
    if (!requested) {
      cancel(nullptr,false); lastGeneration = next;
      return;
    }
    bool enabled = message.value("enabled",false);
    uint32_t id = enabled ? message.value("id",uint32_t(0)) : 0;
    bool acquiring = !enabled && owner.id && !pending.empty() &&
                     message.value("pending_target",uint32_t(0)) == owner.id &&
                     IsWindow(owner.hwnd) && GetFocus() == owner.hwnd &&
                     GetTickCount64()-pendingStarted <= 500;
    // Retain an initial queued gesture only for an explicitly marked handoff to
    // its exact current focus. Policy revocation and other-target leases purge
    // both pending events and held contact state, even before anything was sent.
    if ((sent && (id != leaseId || next != generation)) ||
        (!pending.empty() && ((!enabled && !acquiring) || (enabled && id != owner.id))))
      cancel(nullptr,false);
    lastGeneration = generation = next;
    leaseId = id;
    if (!pending.empty() && id == owner.id) {
      if (!IsWindow(owner.hwnd) || GetFocus() != owner.hwnd ||
          GetTickCount64() - pendingStarted > 500) { cancel(); return; }
      while (!pending.empty()) {
        auto message = std::move(pending.front()); pending.pop_front();
        transmit(std::move(message));
      }
      pendingStarted = 0;
      if (contacts.empty()) { owner = {}; sent = false; }
    }
  }
  void cancel(HWND window = nullptr, bool notify = true) {
    if (window && owner.hwnd != window) return;
    if (notify && sent && leaseId && generation && emit)
      emit({{"type","touch"},{"id",leaseId},{"generation",generation},{"action","cancel"}});
    contacts.clear(); pending.clear(); pendingStarted = 0; sent = false;
    if (owner.hwnd) { suppressedWindow = owner.hwnd; suppressUntil = GetTickCount64()+500; }
    owner = {}; leaseId = 0; generation = 0;
  }
  // Also used by the standalone probe: only the OS sample acquisition is outside
  // this method. Geometry, queueing, slots, cancellation and lease checks are production.
  bool sample(const Target &target, UINT message, UINT pointer, POINT screen, bool canceled = false) {
    if (!negotiated || !requested || !target.enabled || !pointerMessage(message)) return false;
    if (message == WM_POINTERCAPTURECHANGED || canceled) { cancel(target.hwnd); return true; }
    if (message == WM_POINTERDOWN) {
      if (!IsWindow(target.hwnd) || GetFocus() != target.hwnd || target.width <= 0 || target.height <= 0 ||
          target.image.right <= target.image.left || target.image.bottom <= target.image.top) return true;
      if (owner.hwnd && owner.hwnd != target.hwnd) cancel();
      if (contacts.count(pointer) || contacts.size() >= 10) return true;
      POINT local = screen; ScreenToClient(target.hwnd,&local);
      if (!PtInRect(&target.image,local)) return true;
      if (!owner.hwnd) { owner=target; sent=false; }
      uint32_t slot=0;
      while (std::any_of(contacts.begin(),contacts.end(),[&](const auto &c){return c.second.slot==slot;})) ++slot;
      contacts.emplace(pointer,Contact{slot});
      suppressedWindow=target.hwnd; suppressUntil=GetTickCount64()+500;
    }
    auto current=contacts.find(pointer);
    if (current==contacts.end() || owner.hwnd!=target.hwnd) return true;
    POINT local=screen; ScreenToClient(target.hwnd,&local);
    // Use the current image transform, with its matching geometry origin, for
    // each update. The server translates that origin back to its current crop.
    owner.width=target.width; owner.height=target.height; owner.image=target.image;
    owner.originX=target.originX; owner.originY=target.originY;
    if (owner.image.right<=owner.image.left || owner.image.bottom<=owner.image.top) {cancel();return true;}
    const char *phase=message==WM_POINTERDOWN?"down":message==WM_POINTERUP?"up":"motion";
    Json point={{"slot",current->second.slot},{"phase",phase},
                {"x",double(local.x-owner.image.left)*owner.width/(owner.image.right-owner.image.left)},
                {"y",double(local.y-owner.image.top)*owner.height/(owner.image.bottom-owner.image.top)}};
    if(message==WM_POINTERUP) contacts.erase(current);
    packet(std::move(point));
    if(contacts.empty()&&pending.empty()){owner={};sent=false;}
    suppressUntil=GetTickCount64()+500;
    return true;
  }
  bool handle(const Target &target, UINT message, WPARAM wp) {
    if(!negotiated||!requested||!target.enabled||!pointerMessage(message))return false;
    UINT pointer=GET_POINTERID_WPARAM(wp);
    if(message==WM_POINTERCAPTURECHANGED) {
      if(!contacts.count(pointer))return false;
      cancel(target.hwnd);return true;
    }
    POINTER_INPUT_TYPE type{};
    if(!GetPointerType(pointer,&type)||type!=PT_TOUCH)return false;
    POINTER_INFO info{};
    if(!GetPointerInfo(pointer,&info)){cancel(target.hwnd);return true;}
    return sample(target,message,pointer,info.ptPixelLocation,(info.pointerFlags&POINTER_FLAG_CANCELED)!=0);
  }
  bool promotedMouse(HWND window, ULONG_PTR extra) const {
    // Documented Windows pen/touch signature; bit 7 distinguishes touch from pen.
    return negotiated && requested && suppressedWindow==window && GetTickCount64()<=suppressUntil &&
           (extra&0xffffff00)==0xff515700 && (extra&0x80);
  }
};
