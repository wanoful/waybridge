// Actual physical key transitions; composition boundaries are controlled IMM
// messages, not a claim that Wine has a Microsoft IME engine.
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <iostream>
#include <string>

static void key(WORD vk, bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
  if (vk == VK_LEFT || vk == VK_RCONTROL || vk == VK_RMENU)
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  if (SendInput(1, &input, sizeof(input)) != 1)
    ExitProcess(3);
  Sleep(100);
}
int wmain(int argc, wchar_t **argv) {
  if (argc != 2) return 1;
  HWND hwnd = FindWindowW(L"WaybridgeRemote", L"IME key routing probe");
  if (!hwnd) return 2;
  SetForegroundWindow(hwnd);
  Sleep(200);
  std::wstring action = argv[1];
  if (action == L"ready" || action == L"ready-new") {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info) &&
                   info.hwndFocus == hwnd && info.hwndCaret == hwnd &&
                   info.rcCaret.left == (action == L"ready" ? 10 : 30) ? 0 : 4;
  }
  if (action == L"generation-start") {
    key(VK_CONTROL, true);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
  } else if (action == L"old-end") {
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
  } else if (action == L"copy-release") {
    key('C', true); key('C', false);
    key(VK_CONTROL, false);
  } else if (action == L"unicode") {
    for (auto c : std::wstring(L"\u4f60\U0001f642")) {
      INPUT input[2]{};
      for (auto &i : input) {
        i.type = INPUT_KEYBOARD;
        i.ki.wScan = c;
        i.ki.dwFlags = KEYEVENTF_UNICODE;
      }
      input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
      if (SendInput(2, input, sizeof(INPUT)) != 2) return 3;
    }
  } else if (action == L"right-alt-shortcut") {
    key(VK_CONTROL, true); key(VK_RMENU, true);
    key('Q', true); key('Q', false);
    key(VK_RMENU, false); key(VK_CONTROL, false);
  } else if (action == L"type") {
    key('A', true); key('A', false);
  } else if (action == L"shortcut" || action == L"late-character" || action == L"select") {
    key(VK_CONTROL, true);
    WORD vk = action == L"select" ? 'A' : VK_SPACE;
    key(vk, true); key(vk, false);
    key(VK_CONTROL, false);
    if (action == L"late-character")
      SendMessageW(hwnd, WM_CHAR, L' ', LPARAM(1 | (MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC) << 16)));
  } else if (action == L"focus-lost-end") {
    key(VK_CONTROL, true);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    HWND sink = CreateWindowExW(0, L"STATIC", L"IME focus sink", WS_POPUP | WS_VISIBLE,
                                 800, 100, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!sink) return 6;
    SetForegroundWindow(sink); SetFocus(sink); Sleep(200);
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    key(VK_CONTROL, false);
    DestroyWindow(sink);
    SetForegroundWindow(hwnd);
  } else {
    bool shift = action == L"shift";
    key(shift ? VK_SHIFT : VK_CONTROL, true);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    if (action == L"released") key(VK_CONTROL, false);
    if (action == L"cancel") {
      SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 30));
      SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(50, 30));
    } else {
      SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    }
    WORD vk = shift ? VK_LEFT : 'C';
    key(vk, true); key(vk, false);
    if (action != L"released") key(shift ? VK_SHIFT : VK_CONTROL, false);
  }
  Sleep(250);
  return 0;
}
