#pragma once
#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include "native_icon.h"
#include "third_party/json.hpp"
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// One native Shell icon per remote SNI. All methods run on the controller STA.
// IDs, epochs and menu tokens are application identities, never process IDs.
class RemoteTray {
  using Json = nlohmann::json;
  struct Item {
    UINT shellId = 0;
    uint64_t generation = 0, iconRevision = 0;
    Json info;
    NativeIcon icon;
    HICON shellIcon = nullptr;
    bool added = false, version4 = false;
    ~Item() {
      if (shellIcon)
        DestroyIcon(shellIcon);
    }
  };
  HWND owner = nullptr;
  std::function<void(const Json &)> send, emit;
  std::map<std::string, std::unique_ptr<Item>> items;
  uint64_t epoch = 0;
  UINT nextId = 100;
  UINT iconDpi = 0;
  uint64_t badgeRevision = 0;
  bool negotiated = false, enabled = true, readOnly = false;
  std::string pendingMenu;
  uint64_t pendingGeneration = 0;
  HHOOK wheelHook = nullptr;
  inline static RemoteTray *hookTarget = nullptr;
  UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  static std::wstring wide(const std::string &s) {
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size, L'\0');
    if (size)
      MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), size);
    return w;
  }
  NOTIFYICONDATAW data(const Item &item) const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = item.shellId;
    return data;
  }
  void remove(Item &item) {
    if (item.added) {
      auto d = data(item);
      Shell_NotifyIconW(NIM_DELETE, &d);
      item.added = false;
    }
  }
  void paint(Item &item) {
    if (!owner || !negotiated || !enabled || item.info.value("status", "") == "Passive") {
      remove(item);
      return;
    }
    UINT dpi = GetDpiForWindow(owner);
    if (!dpi)
      dpi = 96;
    HICON icon = item.icon.createOwnedIcon(dpi, false);
    // createOwnedIcon is caller-owned. Fallback CopyIcon makes ownership explicit.
    if (!icon)
      icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    auto d = data(item);
    d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    d.uCallbackMessage = CallbackMessage;
    d.hIcon = icon;
    std::wstring title = wide(
        item.info.value("title", item.info.value("app_id", std::string("Remote application"))));
    if (item.info.value("status", "") == "NeedsAttention")
      title = L"● " + title;
    wcsncpy(d.szTip, title.c_str(), 127);
    d.szTip[127] = 0;
    const bool adding = !item.added;
    BOOL ok = Shell_NotifyIconW(item.added ? NIM_MODIFY : NIM_ADD, &d);
    // A duplicate/synthetic TaskbarCreated broadcast can arrive while Explorer
    // still retains this ID. Repair its content without claiming a failure.
    if (!ok && !item.added)
      ok = Shell_NotifyIconW(NIM_MODIFY, &d);
    if (ok) {
      item.added = true;
      if (adding) {
        d.uVersion = NOTIFYICON_VERSION_4;
        item.version4 = Shell_NotifyIconW(NIM_SETVERSION, &d) != FALSE;
        if (GetEnvironmentVariableW(L"WAYBRIDGE_TRAY_TRACE", nullptr, 0))
          std::fprintf(stderr, "Tray shell id=%u version4=%d\n", item.shellId, int(item.version4));
      }
    } else if (emit)
      emit({{"type", "tray_warning"},
            {"message", "Windows Shell could not add a remote tray icon"}});
    if (item.shellIcon)
      DestroyIcon(item.shellIcon);
    item.shellIcon = icon;
  }
  Json action(const std::string &id, const Item &item, const std::string &verb) const {
    return {{"type", "tray_action"},
            {"epoch", epoch},
            {"id", id},
            {"generation", item.generation},
            {"action", verb}};
  }
  void stampActivation(Json &message) {
    if (userActivation) {
      std::string request = userActivation();
      if (!request.empty())
        message["activation_request"] = request;
    }
  }
  static std::wstring label(const std::string &source) {
    // DBusMenu uses '_' mnemonics; native Windows menus use '&'.
    std::wstring result;
    auto text = wide(source);
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == L'&')
        result += L"&&";
      else if (text[i] == L'_') {
        if (i + 1 < text.size() && text[i + 1] == L'_') {
          result += L'_';
          ++i;
        } else
          result += L'&';
      } else
        result += text[i];
    }
    return result;
  }
  static HMENU build(const Json &children, std::map<UINT, int32_t> &actions, UINT &next,
                     size_t &budget, unsigned depth = 0, bool parentEnabled = true) {
    if (!children.is_array() || depth > 8 || children.size() > 256 || !budget)
      return nullptr;
    HMENU menu = CreatePopupMenu();
    if (!menu)
      return nullptr;
    for (const auto &node : children) {
      if (!budget)
        break;
      --budget;
      if (next > 256 || !node.is_object() || !node.value("visible", true))
        continue;
      if (node.value("separator", false)) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        continue;
      }
      bool active = parentEnabled && node.value("enabled", true);
      UINT flags = MF_STRING | (active ? MF_ENABLED : MF_GRAYED) |
                   (node.value("checked", false) ? MF_CHECKED : 0);
      std::wstring text = label(node.value("label", std::string()));
      HMENU submenu = nullptr;
      if (node.value("submenu", false) ||
          (node.contains("children") && !node["children"].empty())) {
        submenu =
            build(node.value("children", Json::array()), actions, next, budget, depth + 1, active);
        if (submenu && GetMenuItemCount(submenu) == 0)
          AppendMenuW(submenu, MF_STRING | MF_GRAYED, 0, L"暂无菜单项");
      }
      UINT command = next++;
      if (submenu)
        AppendMenuW(menu, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), text.c_str());
      else {
        AppendMenuW(menu, flags, command, text.c_str());
        if (active && node.contains("id") && node["id"].is_number_integer())
          actions[command] = node["id"].get<int32_t>();
        if (node.value("toggle", "") == "radio") {
          MENUITEMINFOW info{};
          info.cbSize = sizeof(info);
          info.fMask = MIIM_FTYPE;
          info.fType = MFT_STRING | MFT_RADIOCHECK;
          SetMenuItemInfoW(menu, command, FALSE, &info);
        }
      }
    }
    return menu;
  }
  void showMenu(const Json &message) {
    std::string id = message.value("id", std::string());
    auto found = items.find(id);
    if (found == items.end() || pendingMenu != id ||
        found->second->generation != pendingGeneration ||
        message.value("generation", uint64_t(0)) != pendingGeneration ||
        message.value("epoch", uint64_t(0)) != epoch)
      return;
    pendingMenu.clear();
    if (message.value("fallback", "") == "remote_window") {
      if (emit)
        emit({{"type", "tray_info"}, {"message", "应用菜单已在远端窗口显示"}});
      return;
    }
    if (!message.contains("root") || !message["root"].is_object())
      return;
    std::map<UINT, int32_t> actions;
    UINT next = 1;
    size_t budget = 256;
    HMENU menu = build(message["root"].value("children", Json::array()), actions, next, budget);
    if (!menu)
      return;
    const uint64_t openedEpoch = epoch, generation = pendingGeneration;
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(owner);
    UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x,
                                     point.y, owner, nullptr);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    found = items.find(id);
    if (!selected || !actions.count(selected) || found == items.end() || epoch != openedEpoch ||
        found->second->generation != generation || readOnly || !enabled)
      return;
    Json event = action(id, *found->second, "menu_event");
    event["node"] = actions[selected];
    event["revision"] = message.value("revision", 0u);
    event["menu_token"] = message.value("menu_token", uint64_t(0));
    // The Linux adapter owns its event clock; Windows uptime cannot be used.
    event["timestamp"] = 0u;
    stampActivation(event);
    if (GetEnvironmentVariableW(L"WAYBRIDGE_TRAY_TRACE", nullptr, 0))
      std::fprintf(stderr, "Tray menu select node=%d token=%llu revision=%u\n", actions[selected],
                   (unsigned long long)message.value("menu_token", uint64_t(0)),
                   message.value("revision", 0u));
    send(event);
  }
  static LRESULT CALLBACK wheel(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && hookTarget && (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL)) {
      const auto *event = reinterpret_cast<const MSLLHOOKSTRUCT *>(lp);
      auto &self = *hookTarget;
      if (self.enabled && self.negotiated && !self.readOnly) {
        for (const auto &[id, item] : self.items)
          if (item->added) {
            NOTIFYICONIDENTIFIER identity{};
            identity.cbSize = sizeof(identity);
            identity.hWnd = self.owner;
            identity.uID = item->shellId;
            RECT rect{};
            if (SUCCEEDED(Shell_NotifyIconGetRect(&identity, &rect)) &&
                PtInRect(&rect, event->pt)) {
              Json message = self.action(id, *item, "scroll");
              message["delta"] = int(static_cast<SHORT>(HIWORD(event->mouseData)));
              message["orientation"] = wp == WM_MOUSEHWHEEL ? "horizontal" : "vertical";
              self.send(message);
              break;
            }
          }
      }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
  }

public:
  std::function<std::string()> userActivation;
  static constexpr UINT CallbackMessage = WM_APP + 91;
  RemoteTray() = default;
  RemoteTray(const RemoteTray &) = delete;
  ~RemoteTray() { clear(); }
  void attach(HWND hwnd, std::function<void(const Json &)> sender,
              std::function<void(const Json &)> events) {
    owner = hwnd;
    send = std::move(sender);
    emit = std::move(events);
  }
  void clear() {
    pendingMenu.clear();
    for (auto &[id, item] : items)
      remove(*item);
    items.clear();
    epoch = 0;
    if (wheelHook) {
      UnhookWindowsHookEx(wheelHook);
      wheelHook = nullptr;
    }
    if (hookTarget == this)
      hookTarget = nullptr;
  }
  void configure(bool supported, bool show, bool readonly) {
    negotiated = supported;
    enabled = show;
    readOnly = readonly;
    if (!enabled || readonly) {
      pendingMenu.clear();
      if (owner)
        EndMenu();
    }
    for (auto &[id, item] : items)
      paint(*item);
    if (!supported)
      clear();
  }
  void repaint() {
    for (auto &[id, item] : items)
      paint(*item);
    iconDpi = GetDpiForWindow(owner);
    badgeRevision = IconBadge::state().revision;
  }
  void tick() {
    if (iconDpi != GetDpiForWindow(owner) || badgeRevision != IconBadge::state().revision)
      repaint();
  }
  Json background() const {
    Json result = Json::array();
    if (enabled)
      for (const auto &[id, item] : items) {
        Json value = item->info;
        value.erase("png");
        value["epoch"] = epoch;
        result.push_back(std::move(value));
      }
    return result;
  }
  bool request(const std::string &id, const std::string &verb, uint64_t expectedEpoch = 0,
               uint64_t expectedGeneration = 0) {
    auto found = items.find(id);
    if (found == items.end() || !negotiated || !enabled || readOnly ||
        (verb != "activate" && verb != "menu" && verb != "secondary"))
      return false;
    if ((expectedEpoch && expectedEpoch != epoch) ||
        (expectedGeneration && expectedGeneration != found->second->generation))
      return false;
    const std::string effective =
        verb == "activate" && found->second->info.value("item_is_menu", false) ? "menu" : verb;
    if (effective == "menu") {
      pendingMenu = id;
      pendingGeneration = found->second->generation;
    }
    if (GetEnvironmentVariableW(L"WAYBRIDGE_TRAY_TRACE", nullptr, 0))
      std::fprintf(stderr, "Tray request epoch=%llu generation=%llu action=%s\n",
                   (unsigned long long)epoch, (unsigned long long)found->second->generation,
                   effective.c_str());
    Json message = action(id, *found->second, effective);
    if (effective == "activate" || effective == "secondary" ||
        (effective == "menu" && !found->second->info.value("menu", false)))
      stampActivation(message);
    send(message);
    return true;
  }
  bool receive(const Json &message, ImageDecoder &decoder) {
    std::string type = message.value("type", std::string());
    if (type != "tray_snapshot" && type != "tray_icon" && type != "tray_menu" &&
        type != "tray_status" && type != "tray_error" && type != "tray_warning")
      return false;
    if (!negotiated)
      return true;
    if (type == "tray_status" || type == "tray_error" || type == "tray_warning") {
      if (type == "tray_error")
        std::fprintf(stderr, "Remote tray action failed: %s\n",
                     message.value("message", std::string("unknown error")).c_str());
      if (emit)
        emit(message);
      if (type == "tray_status" && !message.value("available", false))
        clear();
      return true;
    }
    if (type == "tray_icon") {
      auto found = items.find(message.value("id", std::string()));
      if (found == items.end() || message.value("epoch", uint64_t(0)) != epoch ||
          message.value("generation", uint64_t(0)) != found->second->generation ||
          message.value("revision", uint64_t(0)) !=
              found->second->info.value("icon_revision", uint64_t(0)))
        return true;
      auto &item = *found->second;
      item.icon.clear();
      if (message.contains("png") && message["png"].is_string()) {
        const auto &encoded = message["png"].get_ref<const std::string &>();
        DWORD size = 0;
        if (encoded.size() <= 128 * 1024 &&
            CryptStringToBinaryA(encoded.c_str(), DWORD(encoded.size()), CRYPT_STRING_BASE64,
                                 nullptr, &size, nullptr, nullptr) &&
            size <= 96 * 1024) {
          std::vector<uint8_t> png(size);
          if (CryptStringToBinaryA(encoded.c_str(), DWORD(encoded.size()), CRYPT_STRING_BASE64,
                                   png.data(), &size, nullptr, nullptr))
            item.icon.update(png, decoder);
        }
      }
      item.iconRevision = message.value("revision", uint64_t(0));
      paint(item);
      return true;
    }
    if (type == "tray_menu") {
      if (enabled && !readOnly)
        showMenu(message);
      return true;
    }
    if (!message.contains("epoch") || !message["epoch"].is_number_unsigned() ||
        !message.contains("items") || !message["items"].is_array() || message["items"].size() > 64)
      return true;
    uint64_t nextEpoch = message["epoch"].get<uint64_t>();
    if (!nextEpoch)
      return true;
    if (epoch != nextEpoch) {
      clear();
      epoch = nextEpoch;
    }
    std::set<std::string> current;
    for (const auto &info : message["items"]) {
      if (!info.is_object() || !info.contains("id") || !info["id"].is_string() ||
          !info.contains("generation") || !info["generation"].is_number_unsigned())
        continue;
      std::string id = info["id"].get<std::string>();
      if (id.empty() || id.size() > 1024 || !current.insert(id).second)
        continue;
      uint64_t generation = info["generation"].get<uint64_t>();
      if (!generation)
        continue;
      auto &slot = items[id];
      if (!slot) {
        slot = std::make_unique<Item>();
        slot->shellId = nextId++;
        if (nextId > 65535)
          nextId = 100;
      }
      Item &item = *slot;
      if (item.generation != generation) {
        remove(item);
        item.icon.clear();
        if (pendingMenu == id)
          pendingMenu.clear();
      }
      item.generation = generation;
      item.info = info;
      if (item.iconRevision != info.value("icon_revision", uint64_t(0)))
        item.icon.clear();
      paint(item);
    }
    for (auto it = items.begin(); it != items.end();) {
      if (!current.count(it->first)) {
        remove(*it->second);
        it = items.erase(it);
      } else
        ++it;
    }
    if (!items.empty() && !wheelHook) {
      hookTarget = this;
      wheelHook = SetWindowsHookExW(WH_MOUSE_LL, wheel, GetModuleHandleW(nullptr), 0);
    }
    if (emit)
      emit({{"type", "tray_background"}, {"items", background()}});
    return true;
  }
  bool message(UINT message, WPARAM wp, LPARAM lp) {
    if (message == taskbarCreated && taskbarCreated) {
      for (auto &[id, item] : items) {
        item->added = false;
        paint(*item);
      }
      return true;
    }
    if (message == WM_DPICHANGED || message == WM_SETTINGCHANGE || message == WM_DISPLAYCHANGE) {
      repaint();
      return false;
    }
    if (message != CallbackMessage)
      return false;
    const UINT event = LOWORD(lp);
    if (GetEnvironmentVariableW(L"WAYBRIDGE_TRAY_TRACE", nullptr, 0))
      std::fprintf(stderr, "Tray callback v4_id=%u legacy_id=%u event=0x%x\n", HIWORD(lp), UINT(wp),
                   event);
    for (const auto &[key, item] : items) {
      const UINT callbackId = item->version4 ? HIWORD(lp) : UINT(wp);
      if (item->shellId != callbackId)
        continue;
      // Explorer sends compatibility mouse UP messages *and* the version-4
      // semantic event for one click. Handling both toggles applications twice
      // and requests two menus, invalidating the first menu's action token.
      const bool menu = item->version4 ? event == WM_CONTEXTMENU : event == WM_RBUTTONUP;
      const bool activate =
          item->version4 ? (event == NIN_SELECT || event == NIN_KEYSELECT) : event == WM_LBUTTONUP;
      if (menu)
        request(key, "menu");
      else if (activate)
        request(key, item->info.value("item_is_menu", false) ? "menu" : "activate");
      else if (event == WM_MBUTTONUP)
        request(key, "secondary");
      break;
    }
    return true;
  }
};
