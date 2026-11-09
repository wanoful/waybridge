// Actual physical key transitions; composition boundaries are controlled IMM
// messages, not a claim that Wine has a Microsoft IME engine.
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <algorithm>
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
  std::wstring action = argv[1];
  SetForegroundWindow(hwnd);
  Sleep(200);
  if (action == L"ready" || action == L"ready-new") {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info) &&
                   info.hwndFocus == hwnd && info.hwndCaret == hwnd &&
                   info.rcCaret.left == (action == L"ready" ? 10 : 30) ? 0 : 4;
  }
  if (action == L"relative-monitor") {
    // Keep one probe alive before locking: launching/focusing another process
    // while locked can itself release the lock and invalidate the observation.
    HWND cover = nullptr;
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    HANDLE reader = CreateThread(nullptr, 0, [](LPVOID data) -> DWORD {
      auto target = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(data));
      std::string command;
      while (std::getline(std::cin, command)) {
        auto copy = new std::string(command);
        if (!PostThreadMessageW(target, WM_APP + 42, 0, reinterpret_cast<LPARAM>(copy))) {
          delete copy;
          return 1;
        }
        if (command == "quit") break;
      }
      return 0;
    }, reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(GetCurrentThreadId())), 0, nullptr);
    if (!reader) return 7;
    std::cout << "READY" << std::endl;
    while (GetMessageW(&message, nullptr, 0, 0)) {
      if (message.message != WM_APP + 42) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        continue;
      }
      auto copy = reinterpret_cast<std::string *>(message.lParam);
      std::string command = *copy;
      delete copy;
      if (command == "focus") {
        SetForegroundWindow(hwnd);
        Sleep(100);
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info) ||
            info.hwndFocus != hwnd) return 12;
        std::cout << "FOCUS" << std::endl;
      } else if (command == "state") {
        RECT clip{};
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        if (!GetClipCursor(&clip) || !GetCursorInfo(&cursor)) return 9;
        std::cout << "CLIP " << clip.left << ' ' << clip.top << ' ' << clip.right << ' '
                  << clip.bottom << " SHOWING " << ((cursor.flags & CURSOR_SHOWING) ? 1 : 0) << std::endl;
      } else if (command == "cover") {
        // A local no-activate cover permits an outbound claim while preserving
        // the remote source's focus and its already active native pointer lock.
        POINT point{};
        GetCursorPos(&point);
        RECT area{};
        GetClientRect(hwnd, &area);
        POINT first{area.left, area.top}, last{area.right, area.bottom};
        ClientToScreen(hwnd, &first); ClientToScreen(hwnd, &last);
        point.x = std::clamp(point.x, first.x, last.x - 1);
        point.y = std::clamp(point.y, first.y, last.y - 1);
        cover = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"STATIC",
                                L"Relative OLE handoff cover", WS_POPUP | WS_VISIBLE,
                                point.x - 10, point.y - 10, 21, 21, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        if (!cover) return 10;
        std::cout << "COVER" << std::endl;
      } else if (command == "uncover") {
        if (cover) DestroyWindow(cover);
        cover = nullptr;
        std::cout << "UNCOVER" << std::endl;
      } else if (command == "quit") {
        if (cover) DestroyWindow(cover);
        WaitForSingleObject(reader, 1000);
        CloseHandle(reader);
        std::cout << "BYE" << std::endl;
        return 0;
      } else return 11;
    }
    CloseHandle(reader);
    return 0;
  }
  if (action == L"ole-start" || action == L"ole-composition") {
    key(VK_CONTROL, true);
    key('C', true); key('C', false);
    POINT point{60, 70};
    ClientToScreen(hwnd, &point);
    SetCursorPos(point.x, point.y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(200);
    if (action == L"ole-composition")
      SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SetCursorPos(1100, 850);
  } else if (action == L"ole-suspended") {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info) &&
                   !info.hwndCaret ? 0 : 5;
  } else if (action == L"ole-end") {
    // A late composition end must not restore the still physically held Ctrl
    // while the native OLE loop owns input. Release Ctrl inside that loop too.
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    key(VK_CONTROL, false);
    key(VK_ESCAPE, true); key(VK_ESCAPE, false);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(200);
  } else if (action == L"late-copy-character") {
    SendMessageW(hwnd, WM_CHAR, L'c', LPARAM(1 | (MapVirtualKeyW('C', MAPVK_VK_TO_VSC) << 16)));
  } else if (action == L"zero-scan-shortcuts") {
    key(VK_CONTROL, true);
    for (auto vk : {'H', 'I', 'M'}) {
      SendMessageW(hwnd, WM_KEYDOWN, vk, 1);
      SendMessageW(hwnd, WM_CHAR, vk == 'H' ? L'\b' : vk == 'I' ? L'\t' : L'\r', 1);
      SendMessageW(hwnd, WM_KEYUP, vk, 1 | (1LL << 30) | (1LL << 31));
    }
    key(VK_CONTROL, false);
  } else if (action == L"zero-scan-controls" || action == L"ime-control-characters") {
    for (auto vk : {VK_BACK, VK_RETURN}) {
      LPARAM scan = action == L"zero-scan-controls" ? 0 : (LPARAM(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16);
      SendMessageW(hwnd, WM_KEYDOWN, action == L"zero-scan-controls" ? vk : VK_PROCESSKEY, scan | 1);
      SendMessageW(hwnd, WM_CHAR, vk == VK_BACK ? L'\b' : L'\r', scan | 1);
      SendMessageW(hwnd, WM_KEYUP, vk, scan | 1 | (1LL << 30) | (1LL << 31));
    }
  } else if (action == L"empty-composition" || action == L"empty-preedit" || action == L"result-only") {
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    // Reproduce completion/cancellation without a subsequent END notification.
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0,
                 action == L"empty-preedit" ? GCS_COMPSTR :
                 action == L"result-only" ? GCS_RESULTSTR : 0);
    key(VK_BACK, true); key(VK_BACK, false);
    key(VK_RETURN, true); key(VK_RETURN, false);
  } else if (action == L"generation-start") {
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
