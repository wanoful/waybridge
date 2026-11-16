#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  HWND controller = FindWindowExW(HWND_MESSAGE, nullptr, L"WaybridgeControl", L"Waybridge");
  if (!controller) {
    std::puts("no controller");
    return 3;
  }
  std::string command = argv[1];
  if (command == "foreground") {
    HWND active = GetForegroundWindow();
    wchar_t name[100]{}, title[200]{};
    GetClassNameW(active, name, 100);
    GetWindowTextW(active, title, 200);
    return wcscmp(name, L"WaybridgeRemote") == 0 && wcsstr(title, L"Waybridge tray action fixture")
               ? 0
               : 6;
  }
  if (command == "left")
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(NIN_SELECT, 100));
  else if (command == "left-sequence") {
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_LBUTTONDOWN, 100));
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_LBUTTONUP, 100));
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(NIN_SELECT, 100));
  } else if (command == "right-sequence") {
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_RBUTTONDOWN, 100));
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_RBUTTONUP, 100));
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_CONTEXTMENU, 100));
  } else if (command == "legacy-left")
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_LBUTTONUP, 100));
  else if (command == "right")
    SendMessageW(controller, WM_APP + 91, 0, MAKELPARAM(WM_CONTEXTMENU, 100));
  else if (command == "choose") {
    // Only this isolated Wine desktop contains our client's menu. Windows routes
    // the mnemonic through its real menu loop; it is not a fabricated protocol reply.
    HWND menu = FindWindowW(L"#32768", nullptr);
    if (!menu) {
      std::puts("no menu");
      return 4;
    }
    PostMessageW(menu, WM_CHAR, argc > 2 ? argv[2][0] : 'i', 0);
  } else if (command == "stop")
    PostMessageW(controller, WM_CLOSE, 0, 0);
  else
    return 5;
  return 0;
}
