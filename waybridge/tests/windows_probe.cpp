// Exercise native viewer windows and clipboard in a real Windows API
// environment.
#include <windows.h>

#include <cstring>
#include <iostream>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <thread>
#include <vector>
struct DropSource : IDropSource {
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (id == IID_IUnknown || id == IID_IDropSource) {
      *out = this;
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape)
      return DRAGDROP_S_CANCEL;
    return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
int wmain(int argc, wchar_t **argv) {
  if (argc < 2)
    return 1;
  if ((std::wstring(argv[1]) == L"copyfile" || std::wstring(argv[1]) == L"drop") && argc > 2) {
    std::wstring paths = argv[2];
    paths.push_back(L'\0');
    paths.push_back(L'\0');
    size_t size = sizeof(DROPFILES) + paths.size() * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    auto p = static_cast<unsigned char *>(GlobalLock(mem));
    DROPFILES header{};
    header.pFiles = sizeof(header);
    header.fWide = TRUE;
    header.pt = {100, 70};
    memcpy(p, &header, sizeof(header));
    memcpy(p + sizeof(header), paths.data(), paths.size() * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (std::wstring(argv[1]) == L"copyfile") {
      if (!OpenClipboard(nullptr))
        return 2;
      EmptyClipboard();
      SetClipboardData(CF_HDROP, mem);
      CloseClipboard();
    } else {
      GlobalFree(mem);
      HWND hwnd = FindWindowW(L"WaybridgeRemote", nullptr);
      if (!hwnd)
        return 4;
      OleInitialize(nullptr);
      IShellItem *item = nullptr;
      IDataObject *data = nullptr;
      HRESULT hr = SHCreateItemFromParsingName(argv[2], nullptr, IID_PPV_ARGS(&item));
      if (SUCCEEDED(hr)) {
        hr = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        item->Release();
      }
      if (FAILED(hr)) {
        std::cerr << "Shell data object failed " << std::hex << hr << '\n';
        return 6;
      }
      SetForegroundWindow(hwnd);
      POINT point{100, 70};
      ClientToScreen(hwnd, &point);
      SetCursorPos(point.x, point.y);
      mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
      std::thread release([point] {
        Sleep(500);
        SetCursorPos(point.x + 1, point.y);
        Sleep(500);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
      });
      DropSource source;
      DWORD effect = 0;
      hr = DoDragDrop(data, &source, DROPEFFECT_COPY, &effect);
      release.join();
      data->Release();
      OleUninitialize();
      std::cout << "DROP result " << std::hex << hr << " effect " << effect << '\n';
      return hr == DRAGDROP_S_DROP ? 0 : 7;
    }
    return 0;
  }
  if (std::wstring(argv[1]) == L"files") {
    if (!OpenClipboard(nullptr))
      return 2;
    auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) {
      CloseClipboard();
      return 3;
    }
    wchar_t path[32768];
    DragQueryFileW(drop, 0, path, 32768);
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
    CloseClipboard();
    std::cout << out.c_str() << '\n';
    return 0;
  }
  if (std::wstring(argv[1]) == L"clipboard") {
    if (!OpenClipboard(nullptr))
      return 2;
    if (argc > 2) {
      EmptyClipboard();
      size_t n = (wcslen(argv[2]) + 1) * sizeof(wchar_t);
      HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
      void *p = GlobalLock(mem);
      memcpy(p, argv[2], n);
      GlobalUnlock(mem);
      SetClipboardData(CF_UNICODETEXT, mem);
    } else {
      auto mem = GetClipboardData(CF_UNICODETEXT);
      if (!mem) {
        CloseClipboard();
        return 3;
      }
      auto text = static_cast<wchar_t *>(GlobalLock(mem));
      int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
      std::string out(n, '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
      GlobalUnlock(mem);
      std::cout << out.c_str() << '\n';
    }
    CloseClipboard();
    return 0;
  }
  HWND hwnd = nullptr;
  wchar_t title[512]{};
  GetEnvironmentVariableW(L"WAYBRIDGE_TEST_WINDOW", title, 512);
  for (int i = 0; i < 100 && !hwnd; i++) {
    hwnd = FindWindowW(L"WaybridgeRemote", title[0] ? title : nullptr);
    Sleep(100);
  }
  if (!hwnd)
    return 4;
  if (std::wstring(argv[1]) == L"state" && argc > 2) {
    bool ok = std::wstring(argv[2]) == L"maximized"   ? IsZoomed(hwnd)
              : std::wstring(argv[2]) == L"minimized" ? IsIconic(hwnd)
                                                      : !IsZoomed(hwnd) && !IsIconic(hwnd);
    if (ok && std::wstring(argv[2]) == L"maximized" &&
        (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      RECT r;
      GetWindowRect(hwnd, &r);
      GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
      ok = EqualRect(&r, &info.rcWork);
    }
    return ok ? 0 : 8;
  }
  if (std::wstring(argv[1]) == L"restore") {
    ShowWindow(hwnd, SW_RESTORE);
    Sleep(400);
    return 0;
  }
  SetForegroundWindow(hwnd);
  Sleep(300);
  if (std::wstring(argv[1]) == L"geometry") {
    RECT r;
    GetClientRect(hwnd, &r);
    std::cout << "CLIENT " << r.right << ' ' << r.bottom << '\n';
    return 0;
  }
  if (std::wstring(argv[1]) == L"offset" && argc > 3) {
    RECT r;
    GetWindowRect(hwnd, &r);
    SetWindowPos(hwnd, nullptr, r.left + _wtoi(argv[2]), r.top + _wtoi(argv[3]), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    Sleep(250);
    return 0;
  }
  if (std::wstring(argv[1]) == L"popup") {
    HWND popup = FindWindowW(L"WaybridgePopup", nullptr);
    if (argc > 2 && std::wstring(argv[2]) == L"none")
      return popup ? 21 : 0;
    if (!popup)
      return 20;
    if (argc > 2 && std::wstring(argv[2]) == L"nested") {
      bool nested = false;
      for (HWND p = GetTopWindow(nullptr); p; p = GetWindow(p, GW_HWNDNEXT)) {
        wchar_t name[64]{};
        GetClassNameW(p, name, 64);
        if (std::wstring(name) != L"WaybridgePopup")
          continue;
        HWND owner = GetWindow(p, GW_OWNER);
        GetClassNameW(owner, name, 64);
        if (std::wstring(name) == L"WaybridgePopup")
          nested = true;
      }
      return nested ? 0 : 22;
    }
    auto style = GetWindowLongPtrW(popup, GWL_STYLE), ex = GetWindowLongPtrW(popup, GWL_EXSTYLE);
    RECT r;
    GetWindowRect(popup, &r);
    std::cout << "POPUP " << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom << '\n';
    POINT origin{};
    ClientToScreen(hwnd, &origin);
    RECT parent;
    GetClientRect(hwnd, &parent);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info);
    bool outside = r.right > origin.x + parent.right || r.bottom > origin.y + parent.bottom ||
                   r.left < origin.x || r.top < origin.y;
    return (style & WS_POPUP) && !(style & WS_CHILD) && (ex & WS_EX_NOACTIVATE) &&
                   !(ex & WS_EX_APPWINDOW) && outside && info.hwndCapture == hwnd
               ? 0
               : 23;
  }
  if (std::wstring(argv[1]) == L"popup-click" && argc > 2) {
    HWND popup = FindWindowW(L"WaybridgePopup", nullptr);
    if (!popup)
      return 20;
    RECT r;
    GetWindowRect(popup, &r);
    int y = _wtoi(argv[2]);
    if (y < 0)
      y += r.bottom - r.top;
    SetCursorPos((r.left + r.right) / 2, r.top + y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(50);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(350);
    return 0;
  }
  if (std::wstring(argv[1]) == L"frame" && argc > 2) {
    RECT window, client;
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    bool caption = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
    bool expectClient = std::wstring(argv[2]) == L"client";
    std::cout << "FRAME caption=" << caption << " client=" << client.right << 'x' << client.bottom
              << " outer=" << (window.right - window.left) << 'x' << (window.bottom - window.top)
              << '\n';
    if (expectClient) {
      LRESULT hit =
          SendMessageW(hwnd, WM_NCHITTEST, 0,
                       MAKELPARAM(window.left + 2, window.top + (window.bottom - window.top) / 2));
      return !caption && origin.x == window.left && origin.y == window.top &&
                     client.right == window.right - window.left &&
                     client.bottom == window.bottom - window.top && hit == HTLEFT
                 ? 0
                 : 9;
    }
    return caption ? 0 : 9;
  }
  if (std::wstring(argv[1]) == L"shortcut" && argc > 2) {
    UINT vk = argv[2][0];
    INPUT input[4]{};
    for (auto &i : input) i.type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_CONTROL; input[1].ki.wVk = WORD(vk);
    input[2].ki.wVk = WORD(vk); input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].ki.wVk = VK_CONTROL; input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    for (auto &i : input) {
      i.ki.wScan = WORD(MapVirtualKeyW(i.ki.wVk, MAPVK_VK_TO_VSC));
      i.ki.dwFlags |= KEYEVENTF_SCANCODE;
    }
    SendInput(4, input, sizeof(INPUT));
    Sleep(350);
    return 0;
  }
  if (std::wstring(argv[1]) == L"ime-held-copy") {
    auto key = [](WORD vk, bool down) {
      INPUT input{};
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
      input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
      if (SendInput(1, &input, sizeof(input)) != 1) ExitProcess(31);
      Sleep(150);
    };
    key(VK_CONTROL, true);
    // Controlled boundaries exercise state restoration without claiming that
    // Wine implements Microsoft's real composition/candidate engine.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    key('C', true); key('C', false);
    key(VK_CONTROL, false);
    Sleep(350);
    return 0;
  }
  if ((std::wstring(argv[1]) == L"type" || std::wstring(argv[1]) == L"unicode") && argc > 2) {
    for (const wchar_t *p = argv[2]; *p; ++p) {
      INPUT input[2]{};
      for (auto &i : input) i.type = INPUT_KEYBOARD;
      if (std::wstring(argv[1]) == L"unicode") {
        input[0].ki.wScan = input[1].ki.wScan = *p;
        input[0].ki.dwFlags = input[1].ki.dwFlags = KEYEVENTF_UNICODE;
      } else {
        input[0].ki.wVk = input[1].ki.wVk = LOBYTE(VkKeyScanW(*p));
        input[0].ki.wScan = input[1].ki.wScan = WORD(MapVirtualKeyW(input[0].ki.wVk, MAPVK_VK_TO_VSC));
        input[0].ki.dwFlags = input[1].ki.dwFlags = KEYEVENTF_SCANCODE;
      }
      input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
      SendInput(2, input, sizeof(INPUT));
    }
    Sleep(300);
    return 0;
  }
  if (std::wstring(argv[1]) == L"caret") {
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(hwnd, nullptr), &info) || info.hwndCaret != hwnd) return 30;
    std::cout << "CARET " << info.rcCaret.left << ' ' << info.rcCaret.top << ' '
              << info.rcCaret.right << ' ' << info.rcCaret.bottom << '\n';
    return 0;
  }
  if (std::wstring(argv[1]) == L"owner") {
    HWND owner = GetWindow(hwnd, GW_OWNER);
    return owner ? 0 : 12;
  }
  if (std::wstring(argv[1]) == L"click" && argc > 3) {
    RECT r;
    GetClientRect(hwnd, &r);
    int x = _wtoi(argv[2]), y = _wtoi(argv[3]);
    if (x < 0)
      x += r.right;
    LPARAM pos = MAKELPARAM(x, y);
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, pos);
    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    SendMessageW(hwnd, WM_LBUTTONUP, 0, pos);
    Sleep(350);
    return 0;
  }
  if (std::wstring(argv[1]) == L"move") {
    RECT before, after;
    GetWindowRect(hwnd, &before);
    POINT p{100, 16};
    ClientToScreen(hwnd, &p);
    SetCursorPos(p.x, p.y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(350);
    SetCursorPos(p.x + 80, p.y + 60);
    Sleep(350);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(350);
    GetWindowRect(hwnd, &after);
    std::cout << "MOVED " << before.left << ',' << before.top << " -> " << after.left << ','
              << after.top << '\n';
    return before.left != after.left || before.top != after.top ? 0 : 10;
  }
  if (std::wstring(argv[1]) == L"inspect") {
    RECT r;
    GetClientRect(hwnd, &r);
    HDC dc = GetDC(hwnd);
    COLORREF top = GetPixel(dc, r.right / 2, r.bottom / 4),
             bottom = GetPixel(dc, r.right / 2, r.bottom * 3 / 4);
    ReleaseDC(hwnd, dc);
    std::cout << "WINDOW " << r.right << ' ' << r.bottom << " TOP " << int(GetRValue(top)) << ' '
              << int(GetGValue(top)) << ' ' << int(GetBValue(top)) << " BOTTOM "
              << int(GetRValue(bottom)) << ' ' << int(GetGValue(bottom)) << ' '
              << int(GetBValue(bottom)) << '\n';
    return GetRValue(top) > 200 && GetBValue(bottom) > 200 ? 0 : 5;
  }
  if (std::wstring(argv[1]) == L"resize") {
    RECT r{0, 0, 700, 500};
    auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION)
      AdjustWindowRectEx(&r, style, FALSE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    Sleep(500);
    return 0;
  }
  return 1;
}
