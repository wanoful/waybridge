// Only run in the private Wine/Xvfb desktop created by windows_chrome_smoke.py.
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
static std::vector<HWND> remote;
static BOOL CALLBACK collect(HWND hwnd, LPARAM) {
  wchar_t name[128]{}, title[256]{};
  GetClassNameW(hwnd, name, 128);
  GetWindowTextW(hwnd, title, 256);
  if (std::wstring(name) == L"WaybridgeRemote" && IsWindowVisible(hwnd) &&
      std::wstring(title).find(L"Waybridge tab") != std::wstring::npos)
    remote.push_back(hwnd);
  return TRUE;
}
static void refresh() { remote.clear(); EnumWindows(collect, 0); }
static void mouse(DWORD flags) {
  INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = flags;
  if (SendInput(1, &input, sizeof(input)) != 1) ExitProcess(3);
}
static void move(int x, int y) { SetCursorPos(x, y); Sleep(180); }
static void key(WORD code, bool down, DWORD extra = 0) {
  INPUT input{}; input.type = INPUT_KEYBOARD;
  if (extra & KEYEVENTF_UNICODE) input.ki.wScan = code;
  else input.ki.wVk = code;
  input.ki.dwFlags = extra | (down ? 0 : KEYEVENTF_KEYUP);
  if (SendInput(1, &input, sizeof(input)) != 1) ExitProcess(3);
}
static void local(HWND hwnd, int x, int y) {
  POINT p{x, y}; ClientToScreen(hwnd, &p); move(p.x, p.y);
}
static bool count(size_t n) {
  for (int i = 0; i < 100; ++i) { refresh(); if (remote.size() == n) return true; Sleep(80); }
  std::cerr << "remote windows=" << remote.size() << ", expected=" << n << '\n';
  return false;
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 2) return 2;
  std::wstring action(argv[1]);
  refresh();
  if (action == L"ready") return remote.size() == 1 ? 0 : 2;
  if (remote.empty()) return 2;
  if (action == L"split" || action == L"cancel") {
    if (remote.size() != 1) return 4;
    HWND first = remote[0];
    if (GetWindowLongPtrW(first, GWL_STYLE) & WS_CAPTION) return 5;
    SetForegroundWindow(first);
    SetWindowPos(first, HWND_TOP, 40, 40, 960, 640, SWP_SHOWWINDOW);
    Sleep(400);
    local(first, 120, 18); mouse(MOUSEEVENTF_LEFTDOWN);
    local(first, 150, 18); local(first, 160, 80); local(first, 200, 190);
    move(1200, 780);
    if (!count(2)) { mouse(MOUSEEVENTF_LEFTUP); return 6; }
    if (action == L"cancel") {
      INPUT key{}; key.type = INPUT_KEYBOARD; key.ki.wVk = VK_ESCAPE;
      SendInput(1, &key, sizeof(key)); Sleep(100);
      key.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, &key, sizeof(key));
      Sleep(300);
    }
    mouse(MOUSEEVENTF_LEFTUP); Sleep(500);
    if (!count(action == L"cancel" ? 1 : 2)) return 7;
    if (action == L"cancel") {
      // No click/refocus: the original browser must immediately accept keys.
      key(VK_CONTROL, true); key('L', true); key('L', false); key(VK_CONTROL, false);
      Sleep(200);
      for (wchar_t c : std::wstring(L"data:text/html,<title>Waybridge tab after escape</title>")) {
        key(c, true, KEYEVENTF_UNICODE); key(c, false, KEYEVENTF_UNICODE); Sleep(5);
      }
      key(VK_RETURN, true); key(VK_RETURN, false);
      bool navigated = false;
      for (int i = 0; i < 80; ++i) {
        refresh();
        for (auto hwnd : remote) {
          wchar_t title[256]{}; GetWindowTextW(hwnd, title, 256);
          if (std::wstring(title).find(L"after escape") != std::wstring::npos) navigated = true;
        }
        if (navigated) break;
        Sleep(100);
      }
      if (!navigated) return 12;
    }
  } else if (action == L"merge") {
    if (remote.size() != 2) return 8;
    HWND source = nullptr, target = nullptr;
    for (auto hwnd : remote) {
      wchar_t title[256]{}; GetWindowTextW(hwnd, title, 256);
      if (std::wstring(title).find(L"tab A") != std::wstring::npos) source = hwnd;
      else target = hwnd;
    }
    if (!source || !target) return 9;
    SetWindowPos(target, HWND_TOP, 40, 40, 960, 640, SWP_SHOWWINDOW);
    SetWindowPos(source, HWND_TOP, 500, 250, 960, 640, SWP_SHOWWINDOW);
    SetForegroundWindow(source); Sleep(400);
    local(source, 120, 18); mouse(MOUSEEVENTF_LEFTDOWN);
    local(source, 150, 18); move(1100, 800);
    local(target, 140, 18); local(target, 180, 18); local(target, 220, 18);
    Sleep(500); mouse(MOUSEEVENTF_LEFTUP);
    if (!count(1)) return 10;
  } else return 11;
  std::cout << "PASS: Windows Chrome " << std::string(action.begin(), action.end()) << '\n';
  return 0;
}
