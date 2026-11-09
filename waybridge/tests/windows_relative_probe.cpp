// Operates only the fixture in the isolated Wine desktop used by the test runner.
#include <windows.h>
#include <string>
#include <iostream>
static HWND target = nullptr;
static BOOL CALLBACK find(HWND hwnd, LPARAM) {
  wchar_t title[256]{}, klass[128]{};
  GetWindowTextW(hwnd, title, 256);
  GetClassNameW(hwnd, klass, 128);
  if (std::wstring(klass) == L"WaybridgeRemote" && std::wstring(title) == L"Relative fixture lock")
    target = hwnd;
  return TRUE;
}
static void key(WORD code, bool down) {
  INPUT input{}; input.type = INPUT_KEYBOARD;
  input.ki.wVk = code; input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  if (SendInput(1, &input, sizeof(input)) != 1) ExitProcess(3);
}
int wmain(int argc, wchar_t **argv) {
  EnumWindows(find, 0);
  if (argc != 2) return 2;
  std::wstring action = argv[1];
  if (!target && action != L"state") return 2;
  if (action == L"present") {
    return 0;
  } else if (action == L"focus") {
    ShowWindow(target, SW_RESTORE); SetForegroundWindow(target);
    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    Sleep(200);
    PostMessageW(target, WM_SETFOCUS, 0, 0);
    POINT p{120, 80}; ClientToScreen(target, &p); SetCursorPos(p.x, p.y);
    SendMessageW(target, WM_MOUSEMOVE, 0, MAKELPARAM(120, 80));
    SendMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 80));
    SendMessageW(target, WM_LBUTTONUP, 0, MAKELPARAM(120, 80));
  } else if (action == L"setcursor") {
    SendMessageW(target, WM_SETCURSOR, reinterpret_cast<WPARAM>(target), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
  } else if (action == L"motion") {
    INPUT input{}; input.type = INPUT_MOUSE; input.mi.dx = 17; input.mi.dy = -9;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    if (SendInput(1, &input, sizeof(input)) != 1) return 3;
  } else if (action == L"emergency") {
    key(VK_CONTROL, true); key(VK_MENU, true); key(VK_SHIFT, true); key(VK_F12, true);
    key(VK_F12, false); key(VK_SHIFT, false); key(VK_MENU, false); key(VK_CONTROL, false);
  } else if (action == L"blur") {
    PostMessageW(target, WM_KILLFOCUS, 0, 0);
  } else if (action == L"close") {
    PostMessageW(target, WM_CLOSE, 0, 0);
  } else if (action == L"state") {
    RECT clip{}; GetClipCursor(&clip);
    CURSORINFO info{}; info.cbSize = sizeof(info); GetCursorInfo(&info);
    std::cout << "CLIP " << clip.left << ' ' << clip.top << ' ' << clip.right << ' ' << clip.bottom
              << " SHOWING " << ((info.flags & CURSOR_SHOWING) ? 1 : 0)
              << " CURSOR " << (info.hCursor ? 1 : 0) << '\n';
  } else return 4;
  return 0;
}
