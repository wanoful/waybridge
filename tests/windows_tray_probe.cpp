// Exercise production Shell_NotifyIcon and HMENU code with real user32 messages.
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <cstdio>
#include "remote_tray.h"
using Json = nlohmann::json;
RemoteTray tray;
ImageDecoder *decoder = nullptr;
bool activated = false, secondary = false, clicked = false, checkedMenu = false, shellAdded = false;
int phase = 0;
HWND owner = nullptr;
Json item = {{"id", "fixture"},       {"generation", 1u},
             {"icon_revision", 1u},   {"app_id", "org.waybridge.TrayFixture"},
             {"title", "Tray probe"}, {"status", "Active"},
             {"menu", true},          {"item_is_menu", false}};
Json leaf(int id, const char *label) {
  return {{"id", id}, {"label", label}, {"children", Json::array()}};
}
BOOL CALLBACK inspect(HWND hwnd, LPARAM) {
  wchar_t name[64]{};
  GetClassNameW(hwnd, name, 64);
  if (wcscmp(name, L"#32768") != 0)
    return TRUE;
  HMENU menu = reinterpret_cast<HMENU>(SendMessageW(hwnd, 0x01E1, 0, 0));
  if (!menu)
    return TRUE;
  if (GetMenuItemCount(menu) != 6 || checkedMenu)
    return TRUE;
  MENUITEMINFOW radio{};
  radio.cbSize = sizeof(radio);
  radio.fMask = MIIM_FTYPE | MIIM_STATE;
  HMENU sub = GetSubMenu(menu, 3);
  checkedMenu = GetMenuItemCount(menu) == 6 &&
                (GetMenuState(menu, 1, MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)) &&
                (GetMenuState(menu, 2, MF_BYPOSITION) & MF_CHECKED) && sub &&
                GetMenuItemInfoW(sub, 0, TRUE, &radio) && (radio.fType & MFT_RADIOCHECK) &&
                (radio.fState & MFS_CHECKED);
  std::printf("submenu=%p radio_type=%x radio_state=%x checked_all=%d\n", static_cast<void *>(sub),
              radio.fType, radio.fState, checkedMenu);
  PostMessageW(hwnd, WM_CHAR, L'r', 0);
  return FALSE;
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (tray.message(message, wp, lp))
    return 0;
  if (message == WM_APP + 2) {
    std::puts("opening_menu");
    Json children = Json::array({leaf(1, "_Restore"), leaf(2, "Disabled"), leaf(3, "Checked"),
                                 leaf(4, "Nested"), leaf(6, "Quit"), leaf(8, "Disabled parent")});
    children[1]["enabled"] = false;
    children[2]["checked"] = true;
    children[2]["toggle"] = "checkmark";
    auto radio = leaf(5, "Radio");
    radio["checked"] = true;
    radio["toggle"] = "radio";
    children[3]["children"] = Json::array({radio});
    children[5]["enabled"] = false;
    SetTimer(hwnd, 2, 200, nullptr);
    tray.receive({{"type", "tray_menu"},
                  {"epoch", 1u},
                  {"id", "fixture"},
                  {"generation", 1u},
                  {"revision", 1u},
                  {"menu_token", 1u},
                  {"root", {{"children", children}}}},
                 *decoder);
    std::printf("menu_returned error=%lu\n", GetLastError());
    KillTimer(hwnd, 2);
    return 0;
  }
  if (message == WM_TIMER && wp == 2) {
    EnumThreadWindows(GetCurrentThreadId(), inspect, 0);
    return 0;
  }
  if (message == WM_TIMER && wp == 1) {
    if (phase++ == 0) {
      NOTIFYICONIDENTIFIER identity{};
      identity.cbSize = sizeof(identity);
      identity.hWnd = hwnd;
      identity.uID = 100;
      RECT rect{};
      shellAdded = SUCCEEDED(Shell_NotifyIconGetRect(&identity, &rect));
      SendMessageW(hwnd, RemoteTray::CallbackMessage, 0, MAKELPARAM(NIN_SELECT, 100));
      SendMessageW(hwnd, RemoteTray::CallbackMessage, 0, MAKELPARAM(WM_MBUTTONUP, 100));
      SendMessageW(hwnd, RemoteTray::CallbackMessage, 0, MAKELPARAM(WM_CONTEXTMENU, 100));
    } else if (phase > 8) {
      bool unavailable = !tray.request("missing", "activate");
      tray.configure(true, true, true);
      bool readonly = !tray.request("fixture", "activate");
      tray.configure(true, false, false);
      bool disabled = !tray.request("fixture", "activate");
      tray.configure(true, true, false);
      tray.message(RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
      tray.repaint();
      tray.clear();
      bool cleared = tray.background().empty() && !tray.request("fixture", "activate");
      std::printf("activate=%d secondary=%d menu_checked=%d menu_clicked=%d shell_rect=%d "
                  "readonly=%d disabled=%d cleared=%d missing=%d\n",
                  activated, secondary, checkedMenu, clicked, shellAdded, readonly, disabled,
                  cleared, unavailable);
      PostQuitMessage(activated && secondary && checkedMenu && clicked && readonly && disabled &&
                              cleared && unavailable
                          ? 0
                          : 1);
    }
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
int main(int argc, char **argv) {
  if (argc > 1)
    std::freopen(argv[1], "w", stdout);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::puts("probe_started");
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  auto images = std::make_unique<ImageDecoder>();
  decoder = images.get();
  WNDCLASSW wc{};
  wc.lpfnWndProc = procedure;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WaybridgeTrayProbe";
  RegisterClassW(&wc);
  owner = CreateWindowW(wc.lpszClassName, L"Waybridge tray probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        100, 100, 400, 200, nullptr, nullptr, wc.hInstance, nullptr);
  tray.attach(
      owner,
      [](const Json &event) {
        std::printf("sent=%s\n", event.dump().c_str());
        std::string action = event.value("action", "");
        if (action == "activate")
          activated = true;
        if (action == "secondary")
          secondary = true;
        if (action == "menu")
          PostMessageW(owner, WM_APP + 2, 0, 0);
        if (action == "menu_event")
          clicked = event.value("node", 0) == 1 && event.value("epoch", 0) == 1 &&
                    event.value("generation", 0) == 1;
      },
      [](const Json &event) { std::printf("%s\n", event.dump().c_str()); });
  tray.configure(true, true, false);
  tray.receive({{"type", "tray_snapshot"}, {"epoch", 1u}, {"items", Json::array({item})}}, *images);
  SetTimer(owner, 1, 500, nullptr);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  tray.clear();
  DestroyWindow(owner);
  decoder = nullptr;
  images.reset();
  CoUninitialize();
  return int(msg.wParam);
}
