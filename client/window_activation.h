#pragma once
#include <windows.h>
#include <map>
#include <string>
#include "third_party/json.hpp"

// Foreground rights are temporary and belong to the user's local interaction.
// Never attach input queues, synthesize input or use TOPMOST to defeat Windows.
class NativeActivation {
  using Json = nlohmann::json;
  struct Grant { HWND foreground; DWORD input; ULONGLONG expires; bool allowed; };
  struct Pending { Json value; HWND origin; ULONGLONG expires; };
  std::map<std::string, Grant> grants;
  std::map<uint32_t, Pending> pending;
  bool negotiated = false;
  static DWORD inputTime() { LASTINPUTINFO input{}; input.cbSize=sizeof(input); return GetLastInputInfo(&input) ? input.dwTime : 0; }
  static void attention(HWND hwnd) {
    FLASHWINFO info{sizeof(info), hwnd, FLASHW_TRAY, 3, 0}; FlashWindowEx(&info);
  }
  static bool foreground(HWND hwnd) {
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    if (!SetForegroundWindow(hwnd) && GetForegroundWindow() != hwnd) { attention(hwnd); return false; }
    SetActiveWindow(hwnd); SetFocus(hwnd); return true;
  }
  void apply(const Json &value, HWND hwnd, HWND origin) {
    bool allowed = false;
    if (value.contains("request_id") && value["request_id"].is_string()) {
      auto found = grants.find(value["request_id"].get<std::string>());
      if (found == grants.end()) return; // old connection/request is inert
      auto grant = found->second; grants.erase(found);
      allowed = grant.allowed && GetTickCount64() < grant.expires && inputTime() == grant.input &&
                (GetForegroundWindow() == grant.foreground || GetForegroundWindow() == hwnd);
    } else allowed = origin && IsWindow(origin) && GetForegroundWindow() == origin;
    if (allowed) foreground(hwnd); else attention(hwnd);
  }
public:
  void hello(bool value) { negotiated = value; grants.clear(); pending.clear(); }
  bool enabled() const { return negotiated; }
  bool prepare(const std::string &request, bool allowed) {
    for (auto it = grants.begin(); it != grants.end();) {
      if (it->second.expires <= GetTickCount64()) it = grants.erase(it); else ++it;
    }
    if (!negotiated || request.size()!=32 || grants.size()>=32) return false;
    grants[request] = {GetForegroundWindow(), inputTime(), GetTickCount64()+15000, allowed}; return true;
  }
  void request(const Json &message, HWND target, HWND origin, bool shown) {
    if (!negotiated || !target) return;
    auto id = message.value("id", 0u);
    if (shown) apply(message, target, origin);
    else if (pending.size()<64 || pending.count(id)) pending[id] = {message, origin, GetTickCount64()+15000};
  }
  void shown(uint32_t id, HWND hwnd) {
    auto found = pending.find(id); if (found==pending.end()) return;
    auto item=found->second; pending.erase(found);
    if (GetTickCount64()<item.expires) apply(item.value, hwnd, item.origin);
  }
  void closed(uint32_t id) { pending.erase(id); }
  static void user(HWND hwnd, bool allowed) { if (allowed) foreground(hwnd); else attention(hwnd); }
  static bool restoreOwner(HWND closing, HWND owner) {
    if (GetForegroundWindow()!=closing || !owner || !IsWindow(owner) || !IsWindowEnabled(owner) || !IsWindowVisible(owner)) return false;
    // Restore while our closing dialog still owns foreground permission.
    return foreground(owner);
  }
};
