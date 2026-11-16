#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <initializer_list>
constexpr UINT Callback = WM_APP + 91;
HWND owner = nullptr;
POINT original{};
int phase = 0;
int legacyLeft = 0, v4Left = 0, legacyRight = 0, v4Right = 0;
bool overflowInitiallyVisible = false;
BOOL CALLBACK child(HWND hwnd, LPARAM) {
  wchar_t name[100]{}, title[100]{};
  GetClassNameW(hwnd, name, 100);
  GetWindowTextW(hwnd, title, 100);
  RECT r{};
  GetWindowRect(hwnd, &r);
  std::printf("traychild class=%ls visible=%d rect=%ld,%ld,%ld,%ld\n", name, IsWindowVisible(hwnd),
              r.left, r.top, r.right, r.bottom);
  return TRUE;
}
LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == Callback) {
    UINT event = LOWORD(lp);
    std::printf("event=0x%x id=%u wp=%llu\n", event, HIWORD(lp), (unsigned long long)wp);
    legacyLeft += event == WM_LBUTTONUP;
    v4Left += event == NIN_SELECT;
    legacyRight += event == WM_RBUTTONUP;
    v4Right += event == WM_CONTEXTMENU;
    return 0;
  }
  if (message == WM_TIMER) {
    if (phase == 3)
      EnumChildWindows(FindWindowW(L"Shell_TrayWnd", nullptr), child, 0);
    if (phase == 7 || phase == 9 || phase == 12 || phase == 16) {
      NOTIFYICONIDENTIFIER id{};
      id.cbSize = sizeof(id);
      id.hWnd = hwnd;
      id.uID = 100;
      RECT r{};
      HRESULT result = Shell_NotifyIconGetRect(&id, &r);
      std::printf("rect=%ld,%ld,%ld,%ld result=%lx\n", r.left, r.top, r.right, r.bottom, result);
      if (FAILED(result) || r.right <= r.left || r.bottom <= r.top) {
        PostQuitMessage(2);
        return 0;
      }
      BOOL moved = SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
      POINT actual{};
      GetCursorPos(&actual);
      HWND hit = WindowFromPoint(actual);
      wchar_t className[100]{};
      GetClassNameW(hit, className, 100);
      std::printf("cursor=%ld,%ld moved=%d hit=%ls\n", actual.x, actual.y, moved, className);
      if (phase == 7) {
        ++phase;
        return 0;
      }
      HWND hitRoot = GetAncestor(hit, GA_ROOT);
      wchar_t rootClass[100]{};
      GetClassNameW(hitRoot, rootClass, 100);
      if (wcscmp(rootClass, L"Shell_TrayWnd") != 0 &&
          wcscmp(rootClass, L"TopLevelWindowForOverflowXamlIsland") != 0 &&
          wcscmp(rootClass, L"NotifyIconOverflowWindow") != 0) {
        std::printf("blocked_non_tray_root=%ls\n", rootClass);
        PostQuitMessage(3);
        return 0;
      }
      INPUT events[2]{};
      events[0].type = events[1].type = INPUT_MOUSE;
      events[0].mi.dwFlags = phase != 16 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
      events[1].mi.dwFlags = phase != 16 ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
      std::printf("input=%u\n", SendInput(2, events, sizeof(INPUT)));
    }
    if (++phase > 20) {
      SetCursorPos(original.x, original.y);
      std::printf("done legacy_left=%d v4_left=%d legacy_right=%d v4_right=%d\n", legacyLeft,
                  v4Left, legacyRight, v4Right);
      PostQuitMessage(0);
    }
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}
int main(int argc, char **argv) {
  if (argc > 1)
    freopen(argv[1], "w", stdout);
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  GetCursorPos(&original);
  overflowInitiallyVisible =
      IsWindowVisible(FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) ||
      IsWindowVisible(FindWindowW(L"NotifyIconOverflowWindow", nullptr));
  WNDCLASSW wc{};
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WaybridgeTrayEvents";
  RegisterClassW(&wc);
  owner = CreateWindowW(wc.lpszClassName, L"Waybridge isolated callback probe", 0, 0, 0, 0, 0,
                        nullptr, nullptr, wc.hInstance, nullptr);
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = owner;
  data.uID = 100;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  data.uCallbackMessage = Callback;
  wcscpy(data.szTip, L"Waybridge isolated callback probe");
  std::printf("add=%d\n", Shell_NotifyIconW(NIM_ADD, &data));
  data.uVersion = NOTIFYICON_VERSION_4;
  std::printf("version=%d\n", Shell_NotifyIconW(NIM_SETVERSION, &data));
  SetTimer(owner, 1, 700, nullptr);
  MSG m{};
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  Shell_NotifyIconW(NIM_DELETE, &data);
  if (!overflowInitiallyVisible) {
    for (const auto *name : {L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow"}) {
      HWND overflow = FindWindowW(name, nullptr);
      if (overflow && IsWindowVisible(overflow))
        PostMessageW(overflow, WM_KEYDOWN, VK_ESCAPE, 0);
    }
  }
  DestroyWindow(owner);
  SetCursorPos(original.x, original.y);
  return (int)m.wParam;
}
