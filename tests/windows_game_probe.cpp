// Physical mouse/fullscreen checks, restricted to the named remote test window.
#include <windows.h>
#include <windowsx.h>
#include "third_party/json.hpp"
#include <iostream>
#include <string>
using Json = nlohmann::json;
static RECT bounds(HWND hwnd) {
  RECT r{};
  GetWindowRect(hwnd, &r);
  return r;
}
static Json rect(RECT r) { return {r.left, r.top, r.right, r.bottom}; }
struct RestoreInput {
  POINT cursor{};
  HWND foreground = GetForegroundWindow();
  HWND target;
  bool topmost;
  explicit RestoreInput(HWND window)
      : target(window), topmost((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
    GetCursorPos(&cursor);
    SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  ~RestoreInput() {
    if (IsWindow(target))
      SetWindowPos(target, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetCursorPos(cursor.x, cursor.y);
    if (IsWindow(foreground))
      SetForegroundWindow(foreground);
  }
};
static Json state(HWND hwnd) {
  RECT client{};
  GetClientRect(hwnd, &client);
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
  auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  return {{"rect", rect(bounds(hwnd))},
          {"client", {client.right, client.bottom}},
          {"monitor", rect(monitor.rcMonitor)},
          {"dpi", GetDpiForWindow(hwnd)},
          {"foreground", GetForegroundWindow() == hwnd},
          {"caption", (style & WS_CAPTION) != 0},
          {"minimized", IsIconic(hwnd) != FALSE}};
}
int wmain(int argc, wchar_t **argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (argc < 3)
    return 1;
  std::wstring mode = argv[1];
  HWND hwnd = FindWindowW(L"WaybridgeRemote", argv[2]);
  if (!hwnd) {
    struct Search {
      const wchar_t *prefix;
      HWND window = nullptr;
    } search{argv[2]};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
          auto &s = *reinterpret_cast<Search *>(data);
          wchar_t cls[64]{}, title[256]{};
          GetClassNameW(window, cls, 64);
          GetWindowTextW(window, title, 256);
          if (wcscmp(cls, L"WaybridgeRemote") == 0 &&
              wcsncmp(title, s.prefix, wcslen(s.prefix)) == 0) {
            s.window = window;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    hwnd = search.window;
  }
  if (!hwnd || !IsWindowVisible(hwnd))
    return 3;
  RestoreInput restore(hwnd);
  int result = 0;
  if (mode == L"drag") {
    if (argc != 7)
      return 1;
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    // The first mapped frame can precede installation of the CSD handlers.
    Sleep(1500);
    POINT start{_wtoi(argv[3]), _wtoi(argv[4])};
    ClientToScreen(hwnd, &start);
    int dx = _wtoi(argv[5]), dy = _wtoi(argv[6]);
    RECT before = bounds(hwnd);
    SetCursorPos(start.x, start.y);
    Sleep(150);
    wchar_t hit[128]{};
    GetClassNameW(WindowFromPoint(start), hit, 128);
    std::wcout << L"DRAG_START " << hit << L" "
               << SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(start.x, start.y)) << std::endl;
    if (WindowFromPoint(start) != hwnd)
      return 9;
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(150);
    for (int i = 1; i <= 20; ++i) {
      SetCursorPos(start.x + dx * i / 20, start.y + dy * i / 20);
      Sleep(45);
    }
    Sleep(250);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(400);
    RECT after = bounds(hwnd);
    std::cout << Json({{"before", rect(before)},
                       {"after", rect(after)},
                       {"delta", {after.left - before.left, after.top - before.top}},
                       {"requested", {dx, dy}}})
                     .dump()
              << '\n';
    if (abs(after.left - before.left - dx) > 6 || abs(after.top - before.top - dy) > 6)
      result = 2;
  } else if (mode == L"fullscreen") {
    auto full = [&] {
      auto s = state(hwnd);
      return s["rect"] == s["monitor"] && !s["caption"].get<bool>();
    };
    if (IsIconic(hwnd))
      ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    Sleep(300);
    std::cout << "INITIAL " << state(hwnd).dump() << std::endl;
    if (!full())
      return 4;
    std::cout << "BEFORE " << state(hwnd).dump() << std::endl;
    SetWindowPos(hwnd, nullptr, 80, 90, 700, 500, SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(200);
    std::cout << "AFTER_EXTERNAL " << state(hwnd).dump() << std::endl;
    if (!full())
      return 5;
    ShowWindow(hwnd, SW_MINIMIZE);
    Sleep(300);
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    Sleep(700);
    std::cout << "AFTER_RESTORE " << state(hwnd).dump() << std::endl;
    if (!full())
      return 6;
    HWND other = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"Waybridge owned focus fixture",
                                 WS_OVERLAPPEDWINDOW, 100, 100, 300, 150, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    if (!other)
      return 10;
    ShowWindow(other, SW_SHOW);
    DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    bool attached = foregroundThread != GetCurrentThreadId() &&
                    AttachThreadInput(GetCurrentThreadId(), foregroundThread, TRUE);
    SetForegroundWindow(other);
    if (attached)
      AttachThreadInput(GetCurrentThreadId(), foregroundThread, FALSE);
    bool switched = GetForegroundWindow() == other;
    Sleep(400);
    DestroyWindow(other);
    if (IsIconic(hwnd))
      SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    SetForegroundWindow(hwnd);
    Sleep(600);
    std::cout << "AFTER_FOCUS " << state(hwnd).dump() << std::endl;
    if (!switched || !full())
      return 11;
    DWORD_PTR reply{};
    SendMessageTimeoutW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0, SMTO_ABORTIFHUNG, 1000,
                        &reply);
    Sleep(100);
    if (!full())
      return 7;
    SetForegroundWindow(hwnd);
    Sleep(500);
    RECT r = bounds(hwnd);
    HDC screen = GetDC(nullptr);
    wchar_t under[128]{};
    GetClassNameW(
        WindowFromPoint({r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2}), under,
        128);
    std::wcout << L"CENTER_WINDOW " << under << std::endl;
    COLORREF center =
                 GetPixel(screen, r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2),
             edge = GetPixel(screen, r.right - 1, r.top + (r.bottom - r.top) / 2);
    ReleaseDC(nullptr, screen);
    auto s = state(hwnd);
    s["center_pixel"] = {GetRValue(center), GetGValue(center), GetBValue(center)};
    s["right_pixel"] = {GetRValue(edge), GetGValue(edge), GetBValue(edge)};
    std::cout << s.dump() << '\n';
    if (GetGValue(center) < 30 || GetBValue(center) < 60 ||
        abs(int(GetRValue(center)) - int(GetRValue(edge))) > 8 ||
        abs(int(GetGValue(center)) - int(GetGValue(edge))) > 8 ||
        abs(int(GetBValue(center)) - int(GetBValue(edge))) > 8)
      result = 8;
  } else if (mode == L"state") {
    std::cout << state(hwnd).dump() << '\n';
  } else
    return 1;
  return result;
}
